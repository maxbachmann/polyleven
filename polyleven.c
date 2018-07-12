#include <Python.h>
#include <stdint.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define CEILDIV(a,b) (a / b + (a % b != 0))

#define BITMAP_GET(bm,i) (bm[i / 64] & ((uint64_t) 1 << (i % 64)))
#define BITMAP_SET(bm,i) (bm[i / 64] |= ((uint64_t) 1 << (i % 64)))
#define BITMAP_CLR(bm,i) (bm[i / 64] &= ~((uint64_t) 1 << (i % 64)))


/*
 * Basic data structure for handling Unicode objects
 */
struct strbuf {
    void *data;
    int kind;
    Py_ssize_t len;
};

static void strbuf_init(PyObject *unicode, struct strbuf *sb)
{
    sb->data = PyUnicode_DATA(unicode);
    sb->kind = PyUnicode_KIND(unicode);
    sb->len = PyUnicode_GET_LENGTH(unicode);
}

#define STRBUF_READ(sb,idx) (PyUnicode_READ((sb)->kind, (sb)->data, (idx)))

static Py_ssize_t strbuf_find(struct strbuf *sb, Py_UCS4 chr, Py_ssize_t start)
{
    Py_ssize_t idx;
    for (idx = start; idx < sb->len; idx++) {
        if (STRBUF_READ(sb, idx) == chr)
            return idx;
    }
    return -1;
}

/*
 * The model table for mbleven algorithm.
 */
static const char *matrix[] = {
      "r",  NULL,  NULL,  NULL,  NULL,  NULL,  NULL, // 1
      "d",  NULL,  NULL,  NULL,  NULL,  NULL,  NULL,
     "rr",  "id",  "di",  NULL,  NULL,  NULL,  NULL, // 2
     "rd",  "dr",  NULL,  NULL,  NULL,  NULL,  NULL,
     "dd",  NULL,  NULL,  NULL,  NULL,  NULL,  NULL,
    "rrr", "idr", "ird", "rid", "rdi", "dri", "dir", // 3
    "rrd", "rdr", "drr", "idd", "did", "ddi",  NULL,
    "rdd", "drd", "ddr",  NULL,  NULL,  NULL,  NULL,
    "ddd",  NULL,  NULL,  NULL,  NULL,  NULL,  NULL,
};

static const int matrix_row_index[3] = { 0, 2, 5 };

#define MATRIX_COLSIZE 7

/*
 * mbleven algorithm
 */
static Py_ssize_t check_model(struct strbuf *sb1, struct strbuf *sb2, const char *model)
{
    Py_ssize_t i = 0, j = 0, c = 0;

    while (i < sb1->len && j < sb2->len) {
        if (STRBUF_READ(sb1, i) != STRBUF_READ(sb2, j)) {
            switch (model[c]) {
                case 'd':
                    i++;
                    break;
                case 'i':
                    j++;
                    break;
                case 'r':
                    i++;
                    j++;
                    break;
                case '\0':
                    return c + 1;
            }
            c++;
        } else {
            i++;
            j++;
        }
    }
    return c + (sb1->len - i) + (sb2->len - j);
}

static Py_ssize_t mbleven(struct strbuf *sb1, struct strbuf *sb2, Py_ssize_t k)
{
    const char *model;
    int row, col;
    Py_ssize_t res = k + 1;
    Py_ssize_t dst;

    row = matrix_row_index[k - 1] + (sb1->len - sb2->len);
    for (col = 0; col < MATRIX_COLSIZE; col++) {
        model = matrix[row * MATRIX_COLSIZE + col];
        if (model == NULL)
            break;
        dst = check_model(sb1, sb2, model);
        res = MIN(res, dst);
    }

    return res;
}

/*
 * WF1: Optimized for where sb2->len == 1;
 */
static Py_ssize_t wagner_fischer_L1(struct strbuf *sb1, struct strbuf *sb2)
{
    Py_UCS4 c0 = STRBUF_READ(sb2, 0);
    Py_ssize_t i0 = strbuf_find(sb1, c0, 0);
    return sb1->len - (i0 > -1);
}

/*
 * WF2: Optimized for where sb2->len == 2;
 */
static Py_ssize_t wagner_fischer_L2(struct strbuf *sb1, struct strbuf *sb2)
{
    Py_UCS4 c0, c1;
    Py_ssize_t i0, i1;

    c0 = STRBUF_READ(sb2, 0);
    c1 = STRBUF_READ(sb2, 1);

    i0 = strbuf_find(sb1, c0, 0);

    if (i0 == -1 || i0 == sb1->len - 1) {
        i1 = strbuf_find(sb1, c1, 1);
        return sb1->len - (i1 > -1);
    } else {
        i1 = strbuf_find(sb1, c1, i0 + 1);
        return sb1->len - (i1 > -1) - 1;
    }
}

/*
 * An optimized implementation of Wagner-Fischer.
 *
 * The basic idea behind this routine is to avoid filling cells which
 * never produces a sensible edit path; For example, if sb1='abcd' and
 * sb2='xyz', it just computes following cells:
 *
 *        x y z
 *      0 1
 *    a 1 1 2
 *    b 2 2 2 3
 *    c   3 3 3
 *    d     4 4
 *
 * This provides about a 20% speedup on our benchmark.
 *
 * Note that this optimization does not work when sb2->len <= 2.
 * Use wagner_fischer_L* instead for such cases.
 */
static Py_ssize_t wagner_fischer_with_cutoff(struct strbuf *sb1, struct strbuf *sb2, Py_ssize_t *arr)
{
    Py_ssize_t i, j, rpad, lpad;
    Py_ssize_t start, end, top, left, dia;
    Py_UCS4 chr;

    rpad = (sb2->len - 1) / 2;
    lpad = rpad + (sb1->len - sb2->len);

    for (j = 0; j <= rpad; j++)
        arr[j] = j;

    for (i = 1; i <= sb1->len; i++) {
        arr[0] = i - 1;
        chr = STRBUF_READ(sb1, i - 1);

        start = MAX(1, i - lpad);
        dia = arr[start - 1];
        top = arr[start];

        if (chr != STRBUF_READ(sb2, start - 1)) {
            dia = MIN(dia, top);
            dia++;
        }
        arr[start] = dia;
        left = dia;
        dia = top;

        /*
         * Process cells where both the cell above and to the
         * left is filled.
         */
        end = i + rpad - 1;
        if (sb2->len < i + rpad)
            end = sb2->len;

        for (j = start + 1; j <= end; j++) {
            top = arr[j];

            if (chr != STRBUF_READ(sb2, j - 1)) {
                dia = MIN(dia, top);
                dia = MIN(dia, left);
                dia++;
            }
            arr[j] = dia;
            left = dia;
            dia = top;
        }

        if (sb2->len < i + rpad)
            continue;

        if (chr != STRBUF_READ(sb2, end)) {
            dia = MIN(dia, left);
            dia++;
        }
        arr[end + 1] = dia;
    }
    dia = arr[sb2->len];
    return dia;
}

static Py_ssize_t wagner_fischer(struct strbuf *sb1, struct strbuf *sb2)
{
    Py_ssize_t *arr;
    Py_ssize_t res;

    if (!sb2->len)
        return sb1->len;
    if (sb2->len == 1)
        return wagner_fischer_L1(sb1, sb2);
    if (sb2->len == 2)
        return wagner_fischer_L2(sb1, sb2);

    arr = malloc((sb2->len + 1) * sizeof(Py_ssize_t));
    if (arr == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    res = wagner_fischer_with_cutoff(sb1, sb2, arr);
    free(arr);

    return res;
}


/*
 * Myers' bit-parallel algorithm
 *
 * See: G. Myers. "A fast bit-vector algorithm for approximate string
 *      matching based on dynamic programming." Journal of the ACM, 1999.
 */
static uint64_t cmpeach(unsigned char chr, unsigned char *str, uint64_t len)
{
    uint64_t res = 0;
    uint64_t idx;
    for (idx = 0; idx < len; idx++) {
        if (chr == str[idx])
            res |= (uint64_t) 1 << idx;
    }
    return res;
}

static uint64_t myers1999_simple(unsigned char *text, uint64_t len, unsigned char *pat, uint64_t patlen)
{
    uint64_t bitmap[4] = {0, 0, 0, 0};
    uint64_t idx;
    unsigned char chr;
    uint64_t Peq[256];
    uint64_t Eq, Xv, Xh, Ph, Mh, Pv, Mv, Score, Last;

    Mv = 0;
    Pv = ~ (uint64_t) 0;
    Score = patlen;
    Last = (uint64_t) 1 << (patlen - 1);

    for (idx = 0; idx < len; idx++) {
        chr = text[idx];
        if (!BITMAP_GET(bitmap, chr)) {
            BITMAP_SET(bitmap, chr);
            Peq[chr] = cmpeach(chr, pat, patlen);
        }
        Eq = Peq[chr];

        Xv = Eq | Mv;
        Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;

        Ph = Mv | ~ (Xh | Pv);
        Mh = Pv & Xh;

        if (Ph & Last)
            Score += 1;
        if (Mh & Last)
            Score -= 1;

        Ph = (Ph << 1) | 1;
        Mh = (Mh << 1);

        Pv = Mh | ~ (Xv | Ph);
        Mv = Ph & Xv;
    }
    return Score;
}

static uint64_t myers1999_block(unsigned char *text, uint64_t len, unsigned char *pat, uint64_t patlen, uint64_t b, uint64_t *Phc, uint64_t *Mhc)
{
    uint64_t bitmap[4] = {0, 0, 0, 0};
    uint64_t blocklen, idx;
    unsigned char chr, *block;
    uint64_t Peq[256];
    uint64_t Eq, Xv, Xh, Ph, Mh, Pv, Mv, Pb, Mb, Last, Score;

    block = pat + b * 64;
    blocklen = MIN(64, patlen - b * 64);

    Mv = 0;
    Pv = ~ (uint64_t) 0;
    Score = patlen;
    Last = (uint64_t) 1 << (blocklen - 1);

    for (idx = 0; idx < len; idx++) {
        chr = text[idx];
        if (!BITMAP_GET(bitmap, chr)) {
            BITMAP_SET(bitmap, chr);
            Peq[chr] = cmpeach(chr, block, blocklen);
        }
        Eq = Peq[chr];

        Pb = !!BITMAP_GET(Phc, idx);
        Mb = !!BITMAP_GET(Mhc, idx);

        Xv = Eq | Mv;
        Eq |= Mb;
        Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;

        Ph = Mv | ~ (Xh | Pv);
        Mh = Pv & Xh;

        if (Ph & Last) {
            BITMAP_SET(Phc, idx);
            Score++;
        } else {
            BITMAP_CLR(Phc, idx);
        }
        if (Mh & Last) {
            BITMAP_SET(Mhc, idx);
            Score--;
        } else {
            BITMAP_CLR(Mhc, idx);
        }

        Ph = (Ph << 1) | Pb;
        Mh = (Mh << 1) | Mb;

        Pv = Mh | ~ (Xv | Ph);
        Mv = Ph & Xv;
    }
    return Score;
}

static Py_ssize_t myers1999(struct strbuf *sb1, struct strbuf *sb2)
{
    uint64_t i;
    uint64_t vmax, hmax;
    uint64_t *Phc, *Mhc;
    uint64_t res;

    if (sb2->len == 0)
        return sb1->len;

    if (sb2->len <= 64)
        return (Py_ssize_t) myers1999_simple(sb1->data, sb1->len, sb2->data, sb2->len);

    hmax = CEILDIV(sb1->len, 64);
    vmax = CEILDIV(sb2->len, 64);

    Phc = malloc(hmax * sizeof(uint64_t));
    Mhc = malloc(hmax * sizeof(uint64_t));

    if (Phc == NULL || Mhc == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    for (i = 0; i < hmax; i++) {
        Mhc[i] = 0;
        Phc[i] = ~ (uint64_t) 0;
    }

    for (i = 0; i < vmax; i++)
        res = myers1999_block(sb1->data, sb1->len, sb2->data, sb2->len, i, Phc, Mhc);

    free(Phc);
    free(Mhc);

    // Since LevenshteinDistance(sb1, sb2) <= sb1.len,
    // we can safely cast uint64_t to Py_ssize_t.
    return (Py_ssize_t) res;
}


/*
 * Interface function
 */
static PyObject* polyleven_levenshtein(PyObject *self, PyObject *args)
{
    PyObject *u1, *u2;
    struct strbuf sb1, sb2, tmp;
    Py_ssize_t k = -1;
    Py_ssize_t res;

    if (!PyArg_ParseTuple(args, "UU|n", &u1, &u2, &k))
        return NULL;

    strbuf_init(u1, &sb1);
    strbuf_init(u2, &sb2);

    if (sb1.len < sb2.len) {
        tmp = sb1;
        sb1 = sb2;
        sb2 = tmp;
    }

    if (0 <= k && k < sb1.len - sb2.len)
        return PyLong_FromSsize_t(k + 1);

    if (k == 0) {
        res = PyUnicode_Compare(u1, u2) ? 1 : 0;
    } else if (0 < k && k <= 3) {
        res = mbleven(&sb1, &sb2, k);
    } else if (sb1.len < UINT64_MAX && sb1.kind == 1 && sb2.kind == 1) {
        res = myers1999(&sb1, &sb2);
    } else {
        res = wagner_fischer(&sb1, &sb2);
    }

    if (res < 0)
        return NULL;
    if (0 < k && k < res)
        res = k + 1;

    return PyLong_FromSsize_t(res);
}

/*
 * Define an entry point for importing this module
 */
static PyMethodDef polyleven_methods[] = {
    {"levenshtein", polyleven_levenshtein, METH_VARARGS,
     "Compute the levenshtein distance between two strings"}
};

static struct PyModuleDef polyleven_definition = {
    PyModuleDef_HEAD_INIT,
    "polyleven",
    "Yet another library to compute Levenshtain distance",
    -1,
    polyleven_methods
};

PyMODINIT_FUNC PyInit_polyleven(void)
{
    return PyModule_Create(&polyleven_definition);
}
