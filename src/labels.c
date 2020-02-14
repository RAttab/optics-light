/* labels.c
   Rémi Attab (remi.attab@gmail.com), 14 Feb 2020
   FreeBSD-style copyright and disclaimer apply
*/

// -----------------------------------------------------------------------------
// labels
// -----------------------------------------------------------------------------

enum { labels_init_size = 2 };

struct optics_labels
{
    size_t len, cap;
    struct optics_label *labels;
};

size_t optics_labels_len(const struct optics_labels *labels)
{
    return labels->len;
}

const struct optics_label *optics_labels_it(const struct optics_labels *labels)
{
    return labels->labels;
}

struct optics_label *optics_labels_get(struct optics_labels *labels, const char *key)
{
    if (!labels->len) return NULL;
    assert (labels->labels);

    for (size_t i = 0; i < labels->len; ++i) {
        if (!strncmp(key, labels->labels[i].key, optics_name_max_len))
            return &labels->labels[i];
    }

    return NULL;
}

const struct optics_label *optics_labels_find(const struct optics_labels *labels, const char *key)
{
    if (!labels->len) return NULL;
    assert (labels->labels);

    for (size_t i = 0; i < labels->len; ++i) {
        if (!strncmp(key, labels->labels[i].key, optics_name_max_len))
            return &labels->labels[i];
    }

    return NULL;
}

static struct optics_label *optics_label_insert(struct optics_labels *labels, const char *key)
{
    if (labels->len == labels->cap) {
        labels->cap = labels->cap ? labels->cap * 2 : 2;
        struct optics_label *new = calloc(labels->cap, sizeof(labels->labels[0]));
        optics_assert_alloc(new);

        if (labels->len)
            memcpy(new, labels->labels, labels->len * sizeof(labels->labels[0]));

        labels->labels = new;
    }

    struct optics_label *label = &labels->labels[labels->len];
    strlcpy(label->key, key, optics_name_max_len);
    labels->len++;
    return label;
}

static bool optics_labels_set(struct optics_labels *labels, const char *key, const char *val)
{
    struct optics_label *label = optics_labels_get(labels, key);
    if (!label) label = optics_label_insert(labels, key);

    strlcpy(label->val, val, optics_name_max_len);
    return true;
}
