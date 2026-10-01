/* Relaxed geometry only. Never modify the original capture buffers: reads,
 * state barriers and alpha consumers still reconstruct those exact bytes. */
enum { REPLAY_KEEP, REPLAY_CLIP, REPLAY_DROP };
static unsigned clip_quad(const Vertex *vertices, Vertex *out);
static int relaxed_transform(float x, float y, float xs, float ys, float angle) {
    return isfinite(x) && isfinite(y) && isfinite(xs) && isfinite(ys) && isfinite(angle);
}
static int relaxed_quad(const Quad *q) {
    float xmin = q->vertices[0].x, xmax = xmin;
    float ymin = q->vertices[0].y, ymax = ymin;
    unsigned i;
    for (i = 0; i < 6; ++i) {
        const Vertex *v = &q->vertices[i];
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) ||
            !isfinite(v->u) || !isfinite(v->v)) return -1;
        if (v->x < xmin) xmin = v->x;
        if (v->x > xmax) xmax = v->x;
        if (v->y < ymin) ymin = v->y;
        if (v->y > ymax) ymax = v->y;
    }
    if (xmax <= 0 || ymax <= 0 || xmin >= 70 || ymin >= 70) return REPLAY_DROP;
    if (xmin >= 0 && ymin >= 0 && xmax <= 70 && ymax <= 70) return REPLAY_KEEP;
    /* Any corner order and UV layout are allowed. A rotated quad crossing
     * the boundary falls back; a wholly contained rotated quad is replayed. */
    for (i = 0; i < 6; ++i) {
        const Vertex *v = &q->vertices[i];
        if ((v->x != xmin && v->x != xmax) ||
            (v->y != ymin && v->y != ymax)) {
            Vertex clipped[30];
            unsigned j, count = clip_quad(q->vertices, clipped);
            /* A rotated quad can miss a corner of the surface even when its
             * bounding box overlaps. Drop it only if no clipped area remains. */
            for (j = 0; j < count; j += 3) {
                const Vertex *a = &clipped[j], *b = a + 1, *c = a + 2;
                double area = ((double)b->x - a->x) * ((double)c->y - a->y) -
                              ((double)b->y - a->y) * ((double)c->x - a->x);
                if (area != 0) return -1;
            }
            return REPLAY_DROP;
        }
    }
    return REPLAY_CLIP;
}
static Vertex clip_lerp(const Vertex *a, const Vertex *b, double t) {
    Vertex out;
    unsigned shift;
    /* Double intermediates also handle finite endpoints whose subtraction
     * overflows float. Interpolate each triangle's own UVs and RGBA. */
#define LERP(field) out.field = (float)((double)a->field + t * ((double)b->field - a->field))
    LERP(x); LERP(y); LERP(z); LERP(u); LERP(v);
#undef LERP
    out.colour = 0;
    for (shift = 0; shift < 32; shift += 8) {
        unsigned ca = (a->colour >> shift) & 255, cb = (b->colour >> shift) & 255;
        out.colour |= (uint32_t)(ca + t * ((double)cb - ca) + 0.5) << shift;
    }
    return out;
}
static unsigned clip_quad(const Vertex *vertices, Vertex *out) {
    unsigned triangle, total = 0;
    for (triangle = 0; triangle < 2; ++triangle) {
        Vertex buffers[2][8];
        unsigned count = 3, src = 0, edge, i;
        memcpy(buffers[0], vertices + triangle * 3, 3 * sizeof *vertices);
        for (edge = 0; edge < 4 && count; ++edge) {
            unsigned next = 0;
            float boundary = edge & 1 ? 70.0f : 0.0f;
            Vertex a = buffers[src][count - 1];
            float av = edge < 2 ? a.x : a.y;
            int ain = edge & 1 ? av <= boundary : av >= boundary;
            for (i = 0; i < count; ++i) {
                Vertex b = buffers[src][i];
                float bv = edge < 2 ? b.x : b.y;
                int bin = edge & 1 ? bv <= boundary : bv >= boundary;
                if (ain != bin) {
                    double t = ((double)boundary - av) / ((double)bv - av);
                    Vertex intersection = clip_lerp(&a, &b, t);
                    if (edge < 2) intersection.x = boundary;
                    else intersection.y = boundary;
                    buffers[1 - src][next++] = intersection;
                }
                if (bin) buffers[1 - src][next++] = b;
                a = b; av = bv; ain = bin;
            }
            src = 1 - src; count = next;
        }
        /* A triangle clipped by four planes has at most seven vertices,
         * hence five triangles. Both source triangles need at most 30. */
        for (i = 1; i + 1 < count; ++i) {
            out[total++] = buffers[src][0];
            out[total++] = buffers[src][i];
            out[total++] = buffers[src][i + 1];
        }
    }
    return total;
}
