/* Pure FIFO-order predicate shared by the deferred D3D12 alias path and its
 * no-D3D regression test. A later copy must never retarget an earlier read. */
#ifndef PS3RECOMP_RSX_ORDERED_DISPLAY_ALIAS_H
#define PS3RECOMP_RSX_ORDERED_DISPLAY_ALIAS_H

#include <stdint.h>

typedef struct rsx_display_alias_surface {
    uint32_t raw;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
} rsx_display_alias_surface;

/* Strict history aliases are exactly the next equal-stride allocation after
 * two registered, equal-geometry display parents. */
static inline int rsx_display_history_alias_parents(
    const rsx_display_alias_surface* first,
    const rsx_display_alias_surface* second,
    const rsx_display_alias_surface* alias,
    uint32_t* first_raw_out, uint32_t* second_raw_out)
{
    if (!first || !second || !alias || second->raw <= first->raw ||
        first->pitch != second->pitch || first->width != second->width ||
        first->height != second->height || alias->pitch != first->pitch ||
        alias->width != first->width || alias->height != first->height)
        return 0;
    uint64_t stride = (uint64_t)second->raw - first->raw;
    if ((uint64_t)second->raw + stride != alias->raw)
        return 0;
    if (first_raw_out) *first_raw_out = first->raw;
    if (second_raw_out) *second_raw_out = second->raw;
    return 1;
}

/* Records the only permitted metadata parent pair for one alias. Identical
 * entries (including a duplicate traversal) keep the selection; a different
 * matching pair makes ownership ambiguous. */
static inline int rsx_display_history_alias_select_unique_pair(
    const rsx_display_alias_surface* first,
    const rsx_display_alias_surface* second,
    const rsx_display_alias_surface* alias,
    uint32_t* selected_first_raw, uint32_t* selected_second_raw,
    int* have_selected)
{
    uint32_t first_raw, second_raw;
    if (!rsx_display_history_alias_parents(first, second, alias,
                                           &first_raw, &second_raw))
        return 0;
    if (!selected_first_raw || !selected_second_raw || !have_selected)
        return -1;
    if (!*have_selected) {
        *selected_first_raw = first_raw;
        *selected_second_raw = second_raw;
        *have_selected = 1;
        return 1;
    }
    return (*selected_first_raw == first_raw &&
            *selected_second_raw == second_raw) ? 1 : -1;
}

static inline int rsx_ordered_display_alias_copy_precedes(
    uint32_t reader_record, uint32_t copy_record, int copy_prepared,
    int copy_dst_is_display, int copy_dst_slot, int wanted_display_slot)
{
    return copy_record < reader_record && copy_prepared &&
        copy_dst_is_display && copy_dst_slot == wanted_display_slot;
}

/* A live parent may be assembled by adjacent exact copies.  Keep this small
 * pure union proof separate from persistent DisplaySurface coverage: it only
 * considers prepared copies preceding this one reader and can be replayed with
 * `executed_only` to prove the same contributing rectangles actually ran. */
#define RSX_ORDERED_DISPLAY_ALIAS_MAX_COPY_RECTS 32
typedef enum rsx_ordered_display_alias_copy_proof {
    RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR = -1,
    RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_NO = 0,
    RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_YES = 1,
} rsx_ordered_display_alias_copy_proof;

typedef struct rsx_ordered_display_alias_copy_rect {
    uint32_t record;
    uint32_t x, y, w, h;
    int prepared;
    int executed;
    int dst_is_display;
    int dst_slot;
} rsx_ordered_display_alias_copy_rect;

static inline int rsx_ordered_display_alias_copy_union_covers(
    const rsx_ordered_display_alias_copy_rect* copies, uint32_t count,
    uint32_t reader_record, int wanted_display_slot, int executed_only,
    uint32_t want_x, uint32_t want_y, uint32_t want_w, uint32_t want_h,
    int* latest_copy_out)
{
    uint32_t x_cut[RSX_ORDERED_DISPLAY_ALIAS_MAX_COPY_RECTS * 2 + 2];
    uint32_t n = 2;
    int latest = -1;
    uint64_t want_x_end, want_y_end;
    if (latest_copy_out) *latest_copy_out = -1;
    if (!copies || count > RSX_ORDERED_DISPLAY_ALIAS_MAX_COPY_RECTS ||
        !want_w || !want_h)
        return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR;
    want_x_end = (uint64_t)want_x + want_w;
    want_y_end = (uint64_t)want_y + want_h;
    if (want_x_end > UINT32_MAX || want_y_end > UINT32_MAX)
        return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR;
    x_cut[0] = want_x;
    x_cut[1] = (uint32_t)want_x_end;
    for (uint32_t i = 0; i < count; ++i) {
        const rsx_ordered_display_alias_copy_rect* c = &copies[i];
        uint64_t x_end, y_end;
        if (!rsx_ordered_display_alias_copy_precedes(
                reader_record, c->record, c->prepared, c->dst_is_display,
                c->dst_slot, wanted_display_slot) ||
            (executed_only && !c->executed))
            continue;
        x_end = (uint64_t)c->x + c->w;
        y_end = (uint64_t)c->y + c->h;
        if (!c->w || !c->h || x_end > UINT32_MAX || y_end > UINT32_MAX)
            return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR;
        if (c->x >= want_x_end || c->y >= want_y_end ||
            x_end <= want_x || y_end <= want_y)
            continue;
        x_cut[n++] = c->x < want_x ? want_x : c->x;
        x_cut[n++] = x_end > want_x_end ? (uint32_t)want_x_end : (uint32_t)x_end;
        if ((int)c->record > latest) latest = (int)c->record;
    }
    for (uint32_t i = 1; i < n; ++i) {
        uint32_t v = x_cut[i], j = i;
        while (j && x_cut[j - 1] > v) { x_cut[j] = x_cut[j - 1]; --j; }
        x_cut[j] = v;
    }
    for (uint32_t xi = 0; xi + 1 < n; ++xi) {
        uint32_t left = x_cut[xi], right = x_cut[xi + 1], covered = want_y;
        if (left == right) continue;
        for (;;) {
            uint32_t best = covered;
            for (uint32_t i = 0; i < count; ++i) {
                const rsx_ordered_display_alias_copy_rect* c = &copies[i];
                uint64_t x_end, y_end;
                if (!rsx_ordered_display_alias_copy_precedes(
                        reader_record, c->record, c->prepared, c->dst_is_display,
                        c->dst_slot, wanted_display_slot) ||
                    (executed_only && !c->executed))
                    continue;
                x_end = (uint64_t)c->x + c->w;
                y_end = (uint64_t)c->y + c->h;
                if (!c->w || !c->h || x_end > UINT32_MAX || y_end > UINT32_MAX)
                    return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR;
                if (c->x > left || x_end < right || c->y > covered || y_end <= covered)
                    continue;
                if (y_end > best) best = y_end > want_y_end ?
                    (uint32_t)want_y_end : (uint32_t)y_end;
            }
            if (best == covered) return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_NO;
            covered = best;
            if (covered >= want_y_end) break;
        }
    }
    if (latest < 0) return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_NO;
    if (latest_copy_out) *latest_copy_out = latest;
    return RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_YES;
}

/* A history alias has no fixed parent. It becomes live only when exactly one
 * parent DisplaySurface has a preceding exact-copy producer. */
static inline int rsx_ordered_display_alias_unique_owner(
    int first_slot, int second_slot, int first_prior, int second_prior)
{
    int first_live = first_slot >= 0 && first_prior;
    int second_live = second_slot >= 0 && second_prior;
    return first_live == second_live ? -1 : (first_live ? first_slot : second_slot);
}

/* Corrupt/capped proof for either existing parent is not evidence that the
 * other parent owns the alias. Keep the read on history/null instead. */
static inline int rsx_ordered_display_alias_unique_owner_proofs(
    int first_slot, int second_slot, int first_proof, int second_proof)
{
    if ((first_slot >= 0 && first_proof == RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR) ||
        (second_slot >= 0 && second_proof == RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_ERROR))
        return -1;
    return rsx_ordered_display_alias_unique_owner(
        first_slot, second_slot,
        first_proof == RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_YES,
        second_proof == RSX_ORDERED_DISPLAY_ALIAS_COPY_PROOF_YES);
}

/* A recognized display read is terminal: live ownership, completed history,
 * or an explicit null SRV. It must not fall through to another resource class. */
typedef enum rsx_ordered_display_alias_route {
    RSX_ORDERED_DISPLAY_ALIAS_ROUTE_NULL = 0,
    RSX_ORDERED_DISPLAY_ALIAS_ROUTE_HISTORY,
    RSX_ORDERED_DISPLAY_ALIAS_ROUTE_LIVE,
} rsx_ordered_display_alias_route;

static inline rsx_ordered_display_alias_route
rsx_ordered_display_alias_terminal_route(int live_owner, int history_available)
{
    return live_owner >= 0 ? RSX_ORDERED_DISPLAY_ALIAS_ROUTE_LIVE :
        (history_available ? RSX_ORDERED_DISPLAY_ALIAS_ROUTE_HISTORY :
                             RSX_ORDERED_DISPLAY_ALIAS_ROUTE_NULL);
}

#endif /* PS3RECOMP_RSX_ORDERED_DISPLAY_ALIAS_H */
