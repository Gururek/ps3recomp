#ifndef RSX_FP_PROBE_INJECT_H
#define RSX_FP_PROBE_INJECT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Unsigned 1-8 digit hexadecimal parser: no whitespace, sign, or overflow. */
static int rsx_fp_probe_parse_u32_hex(const char* value, uint32_t* out_value)
{
    const char* p = value;
    if (!p || !out_value) return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t parsed = 0;
    unsigned digits = 0;
    for (; *p; ++p) {
        unsigned nibble;
        if (*p >= '0' && *p <= '9') nibble = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') nibble = (unsigned)(*p - 'a') + 10;
        else if (*p >= 'A' && *p <= 'F') nibble = (unsigned)(*p - 'A') + 10;
        else return 0;
        if (digits++ == 8) return 0;
        parsed = (parsed << 4) | nibble;
    }
    if (!digits) return 0;
    *out_value = parsed;
    return 1;
}

static int rsx_fp_probe_target_matches(const char* value, uint32_t fp_addr)
{
    uint32_t target;
    return rsx_fp_probe_parse_u32_hex(value, &target) && target == fp_addr;
}

/* Inject after all translated FP instructions, directly before generated
 * exports/alpha test.  Returns zero without changing hlsl if any prerequisite
 * declaration, marker, or capacity check fails. */
static int rsx_fp_probe_stagegrid_inject(char* hlsl, size_t capacity)
{
    static const char marker[] = "    PSOut _po;";
    static const char injected[] =
        "    int _tj_fp_stage_band = (int)(input.position.x / (1280.0 / 7.0));\n"
        "    _tj_fp_stage_band = min(6, max(0, _tj_fp_stage_band));\n"
        "    float3 _tj_fp_stage_rgb;\n"
        "    if (_tj_fp_stage_band == 0) _tj_fp_stage_rgb = rsx_tex0.Sample(rsx_samp0, (input.tc1.xy / input.tc1.w) * rsx_texscale[0].xy).rgb;\n"
        "    else if (_tj_fp_stage_band == 1) _tj_fp_stage_rgb = float3(frac(input.tc1.xy / input.tc1.w), 0);\n"
        "    else if (_tj_fp_stage_band == 2) _tj_fp_stage_rgb = r[0].xxx;\n"
        "    else if (_tj_fp_stage_band == 3) _tj_fp_stage_rgb = rsx_tex1.Sample(rsx_samp1, input.tc0.xy * rsx_texscale[1].xy).rgb * input.col0.rgb;\n"
        "    else if (_tj_fp_stage_band == 4) _tj_fp_stage_rgb = h[7].www;\n"
        "    else if (_tj_fp_stage_band == 5) _tj_fp_stage_rgb = h[2].rgb;\n"
        "    else _tj_fp_stage_rgb = h[0].rgb;\n"
        "    PSOut _tj_fp_stage_out = (PSOut)0;\n"
        "    _tj_fp_stage_out.c0 = float4(_tj_fp_stage_rgb, 1);\n"
        "    return _tj_fp_stage_out;\n";
    size_t length = 0;
    while (length < capacity && hlsl[length]) ++length;
    if (length == capacity ||
        !strstr(hlsl, "float4 position : SV_POSITION") ||
        !strstr(hlsl, "float4 col0 : COLOR0") ||
        !strstr(hlsl, "tc0:TEXCOORD0") || !strstr(hlsl, "tc1:TEXCOORD1") ||
        !strstr(hlsl, "rsx_tex0 : register(t0)") ||
        !strstr(hlsl, "rsx_tex1 : register(t1)") ||
        !strstr(hlsl, "rsx_samp0 : register(s0)") ||
        !strstr(hlsl, "rsx_samp1 : register(s1)") ||
        !strstr(hlsl, "rsx_texscale[4]") ||
        !strstr(hlsl, "float4 r[48]; float4 h[48];")) return 0;
    char* export_marker = strstr(hlsl, marker);
    size_t insert_size = sizeof(injected) - 1;
    if (!export_marker || insert_size > capacity - length - 1) return 0;
    size_t offset = (size_t)(export_marker - hlsl);
    memmove(hlsl + offset + insert_size, hlsl + offset, length - offset + 1);
    memcpy(hlsl + offset, injected, insert_size);
    return 1;
}

/* Target FP late-path grid.  This samples values which still exist directly
 * before the generated export, so it does not need to alter any translated
 * instruction.  A changed input/decompiler declaration fails closed. */
static int rsx_fp_probe_lategrid_inject(char* hlsl, size_t capacity)
{
    static const char marker[] = "    PSOut _po;";
    static const char injected[] =
        "    int _tj_fp_late_band = (int)(input.position.x / (1280.0 / 7.0));\n"
        "    _tj_fp_late_band = min(6, max(0, _tj_fp_late_band));\n"
        "    float3 _tj_fp_late_rgb;\n"
        "    if (_tj_fp_late_band == 0) _tj_fp_late_rgb = rsx_tex1.Sample(rsx_samp1, input.tc0.xy * rsx_texscale[1].xy).rgb * input.col0.rgb;\n"
        "    else if (_tj_fp_late_band == 1) _tj_fp_late_rgb = h[7].www;\n"
        "    else if (_tj_fp_late_band == 2) _tj_fp_late_rgb = h[2].rgb;\n"
        "    else if (_tj_fp_late_band == 3) _tj_fp_late_rgb = input.fog.yyy;\n"
        "    else if (_tj_fp_late_band == 4) _tj_fp_late_rgb = h[2].www;\n"
        "    else if (_tj_fp_late_band == 5) _tj_fp_late_rgb = r[1].www;\n"
        "    else _tj_fp_late_rgb = h[0].rgb;\n"
        "    PSOut _tj_fp_late_out = (PSOut)0;\n"
        "    _tj_fp_late_out.c0 = float4(_tj_fp_late_rgb, 1);\n"
        "    return _tj_fp_late_out;\n";
    size_t length = 0;
    while (length < capacity && hlsl[length]) ++length;
    if (length == capacity ||
        !strstr(hlsl, "float4 position : SV_POSITION") ||
        !strstr(hlsl, "float4 col0 : COLOR0") ||
        !strstr(hlsl, "float4 fog : FOG") ||
        !strstr(hlsl, "tc0:TEXCOORD0") ||
        !strstr(hlsl, "rsx_tex1 : register(t1)") ||
        !strstr(hlsl, "rsx_samp1 : register(s1)") ||
        !strstr(hlsl, "rsx_texscale[4]") ||
        !strstr(hlsl, "float4 r[48]; float4 h[48];")) return 0;
    char* export_marker = strstr(hlsl, marker);
    size_t insert_size = sizeof(injected) - 1;
    if (!export_marker || insert_size > capacity - length - 1) return 0;
    size_t offset = (size_t)(export_marker - hlsl);
    memmove(hlsl + offset + insert_size, hlsl + offset, length - offset + 1);
    memcpy(hlsl + offset, injected, insert_size);
    return 1;
}

/* Target-program-only TXP repair. Match the full generated t0/TEXCOORD1
 * expression, rather than any generic TXP lowering, so a changed decompiler
 * spelling or another projected sample fails closed without mutation. */
static int rsx_fp_probe_txp_zeroq_inject(char* hlsl, size_t capacity)
{
    static const char old_sample[] =
        "rsx_tex0.Sample(rsx_samp0, ((input.tc1).xyzw).xy / ((input.tc1).xyzw).w * rsx_texscale[0].xy)";
    static const char new_sample[] =
        "rsx_tex0.Sample(rsx_samp0, _tj_txp_zeroq(((input.tc1).xyzw).xy, ((input.tc1).xyzw).w, rsx_texscale[0].xy))";
    static const char main_marker[] = "PSOut main(PSInput input) {\n";
    static const char helper[] =
        "float2 _tj_txp_zeroq(float2 xy, float q, float2 scale) {\n"
        "    return q == 0.0 ? float2(0,0) : (xy / q) * scale;\n"
        "}\n";
    size_t length = 0;
    while (length < capacity && hlsl[length]) ++length;
    if (length == capacity) return 0;
    char* sample = strstr(hlsl, old_sample);
    char* main = strstr(hlsl, main_marker);
    if (!sample || !main || strstr(sample + sizeof(old_sample) - 1, old_sample)) return 0;
    size_t old_size = sizeof(old_sample) - 1;
    size_t new_size = sizeof(new_sample) - 1;
    size_t helper_size = sizeof(helper) - 1;
    if (new_size - old_size > capacity - length - 1 ||
        helper_size > capacity - (length + new_size - old_size) - 1) return 0;
    size_t sample_offset = (size_t)(sample - hlsl);
    memmove(hlsl + sample_offset + new_size, hlsl + sample_offset + old_size,
            length - sample_offset - old_size + 1);
    memcpy(hlsl + sample_offset, new_sample, new_size);
    length += new_size - old_size;
    main = strstr(hlsl, main_marker);
    size_t main_offset = (size_t)(main - hlsl);
    memmove(hlsl + main_offset + helper_size, hlsl + main_offset,
            length - main_offset + 1);
    memcpy(hlsl + main_offset, helper, helper_size);
    return 1;
}

/* Screen-space t0 scan for one target FP.  All prerequisites are generated
 * declarations; a changed decompiler spelling leaves HLSL untouched. */
static int rsx_fp_probe_t0_screen_inject(char* hlsl, size_t capacity)
{
    static const char entry_marker[] = "PSOut main(PSInput input) {\n";
    static const char injected[] =
        "    float2 _tj_fp_probe_t0_uv = saturate(input.position.xy * float2(1.0/1280.0,1.0/720.0)) * rsx_texscale[0].xy;\n"
        "    float4 _tj_fp_probe_t0_s = rsx_tex0.Sample(rsx_samp0, _tj_fp_probe_t0_uv);\n"
        "    PSOut _tj_fp_probe_t0_out = (PSOut)0;\n"
        "    _tj_fp_probe_t0_out.c0 = float4(_tj_fp_probe_t0_s.r, _tj_fp_probe_t0_s.g, max(_tj_fp_probe_t0_s.b, _tj_fp_probe_t0_s.a), 1);\n"
        "    return _tj_fp_probe_t0_out;\n";
    size_t length = 0;
    while (length < capacity && hlsl[length]) ++length;
    if (length == capacity || !strstr(hlsl, "float4 position : SV_POSITION") ||
        !strstr(hlsl, "rsx_tex0 : register(t0)") ||
        !strstr(hlsl, "rsx_samp0 : register(s0)") ||
        !strstr(hlsl, "rsx_texscale[4]")) return 0;
    char* entry = strstr(hlsl, entry_marker);
    size_t insert_size = sizeof(injected) - 1;
    if (!entry || insert_size > capacity - length - 1) return 0;
    size_t offset = (size_t)(entry - hlsl) + sizeof(entry_marker) - 1;
    memmove(hlsl + offset + insert_size, hlsl + offset, length - offset + 1);
    memcpy(hlsl + offset, injected, insert_size);
    return 1;
}

#endif
