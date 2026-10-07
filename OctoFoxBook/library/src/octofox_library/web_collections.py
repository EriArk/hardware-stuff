"""Editorial shelves over existing OPDS metadata; no file copies or user writes.

Exact, indexed facets keep future imports in the collection automatically. A
surname such as Kim, an arbitrary ID range, or the generic 'Asia' tag is not
enough: those also match unrelated authors and non-fiction.
"""

COLLECTIONS = {
    "korean-elves": {
        "id": "korean-elves",
        "title": "Корейские эльфы",
        "description": "Дорамы, романтика, уютные истории и корейское волшебство.",
        "image": "/korean-elves.png",
        "facets": {
            "tag": ("корейские новеллы", "корейская литература", "корейское фэнтези"),
            "series": ("лучшие дорамы", "young adult. корейская волна"),
        },
    },
}


def collection_filter(key, owner):
    """Return a parameterized predicate for the books table aliased as b."""
    definition = COLLECTIONS[key]
    branches, params = [], [owner]
    for kind, values in definition["facets"].items():
        branches.append("(f.kind=? AND f.value IN (" + ",".join("?" for _ in values) + "))")
        params.extend((kind, *values))
    return (
        "b.id IN (SELECT f.book FROM book_facets f WHERE f.owner=? AND ("
        + " OR ".join(branches) + "))",
        params,
    )
