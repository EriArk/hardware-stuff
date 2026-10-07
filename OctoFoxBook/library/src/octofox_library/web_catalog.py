"""Search taxonomy for the website; source metadata and OPDS are left intact.

Only explicit genre codes become genres. Free-form subjects are tags, grouped
for browsing only: a word in a title/tag never assigns a book to a genre.
"""

from __future__ import annotations

import re
import unicodedata

from octofox_library.opds_genres import GENRE_GROUPS, GENRES_BY_CODE


def normalized(value: str) -> str:
    return " ".join(unicodedata.normalize("NFKC", value).casefold().replace("ё", "е").split())


GENRES = {code: (group.slug, genre.title) for code, (group, genre) in GENRES_BY_CODE.items()}
GENRES.update(
    {
        "sf_fantasy": ("fantasy", "Фэнтези"),
        "sf_horror": ("horror", "Ужасы"),
        "sf_mystic": ("horror", "Мистика"),
        "sf_epic": ("fantasy", "Эпическое фэнтези"),
        "sf_etc": ("science-fiction", "Другая фантастика"),
        "sf_stimpank": ("science-fiction", "Стимпанк"),
        "sf_postapocalyptic": ("science-fiction", "Постапокалиптика"),
        "sf_litrpg": ("fantasy", "ЛитРПГ"),
        "sf_space_opera": ("science-fiction", "Космоопера"),
        "sf_technofantasy": ("fantasy", "Технофэнтези"),
        "fantasy_dark": ("fantasy", "Тёмное фэнтези"),
        "dragon_fantasy": ("fantasy", "Фэнтези о драконах"),
        "fairy_fantasy": ("fantasy", "Сказочное фэнтези"),
        "vampire_book": ("horror", "Книги о вампирах"),
        "horror_fantasy": ("horror", "Мистическое фэнтези"),
        "gothic_novel": ("horror", "Готический роман"),
        "foreign_love": ("romance", "Зарубежные любовные романы"),
        "foreign_adventure": ("adventures", "Зарубежные приключения"),
        "adv_modern": ("adventures", "Современные приключения"),
        "prose_magic": ("prose", "Магический реализм"),
        "prose_military": ("prose", "Военная проза"),
        "prose_abs": ("prose", "Литература абсурда"),
        "literature_18": ("prose", "Литература XVIII века"),
        "antique_ant": ("prose", "Античная литература"),
        "antique_east": ("prose", "Древневосточная литература"),
        "antique_myths": ("prose", "Мифы и легенды"),
        "foreign_antique": ("prose", "Древняя зарубежная литература"),
        "foreign_publicism": ("non-fiction", "Зарубежная публицистика"),
        "foreign_poetry": ("poetry-humor", "Зарубежная поэзия"),
        "foreign_dramaturgy": ("poetry-humor", "Зарубежная драматургия"),
        "sci_philology": ("science-education", "Филология"),
        "cinema_theatre": ("non-fiction", "Кино и театр"),
        "music_dancing": ("non-fiction", "Музыка и танец"),
    }
)
GENRE_ALIASES = {
    "sf_fantasy_city": "city_fantasy",
    "sf_cyber_punk": "sf_cyberpunk",
    "sf_fantasy_irony": "humor_fantasy",
    "sf_irony": "sf_humor",
    "romance_sf": "love_sf",
    "romance_fantasy": "love_fantasy",
    "knigi_fentezi_zarubezhnye": "foreign_fantasy",
    "boevaya_fantastika": "sf_action",
    "foreign_action": "det_action",
    "sci_phys": "sci_physics",
    "sci_popular": "popular_science",
    "literature_classics": "prose_classic",
    "literature_su_classics": "prose_su_classics",
    "horror_vampires": "vampire_book",
    "fantasy_alt_hist": "historical_fantasy",
    "literature_adv": "adventure",
    "literature_western": "adv_western",
    "folk_tale": "child_tale",
    "entert_humor": "humor",
    "knigi_sovremennaya_proza_zarubezhnaya": "foreign_contemporary",
}
GENRE_CATEGORIES = {g.slug: g.title for g in GENRE_GROUPS} | {"horror": "Ужасы и мистика"}
TAG_CATEGORIES = {
    "magic": "Магия и существа",
    "worlds": "Миры, места и эпохи",
    "adventure": "Приключения и конфликты",
    "relationships": "Отношения и чувства",
    "society": "История и общество",
    "style": "Тематика и поджанры",
    "audience": "Аудитория и издания",
    "other": "Другие темы",
}
TAG_RULES = (
    (
        "audience",
        r"young.adult|подрост|детск|молодеж|молодёж|издани|иллюстр|сборник|рассказ|новелл|экраниз|бестселлер|преми|^tn_|short_story|great_story|network_literature",
    ),
    (
        "style",
        r"фэнтези|фантастик|триллер|детектив|киберпанк|литрпг|litrpg|реализм|хоррор|антиутоп|космоопер|стимпанк|утопи|проза",
    ),
    (
        "magic",
        r"маги|волшеб|дракон|вампир|ведьм|демон|монстр|оборотн|призрак|бог[иов]|божеств|эльф|колдов|сверхспособ|нежит|чудовищ|артефакт|пророч",
    ),
    (
        "worlds",
        r"мир|космо|цивилизац|будущ|прошло|эпох|времен|планет|инопланет|пришельц|вселен|зон[аыу]|постапокалип|земл|город|росси|англи|япон|франц|амери|ссср|средневек",
    ),
    (
        "relationships",
        r"любов|романт|отношени|семь|семей|дружб|чувств|судьб|взрослен|одиноч|психолог|страст|свадьб|брак|сердц|измен|предатель|ревност",
    ),
    (
        "society",
        r"истор|войн|военн|общество|социаль|власт|полит|спецслужб|революц|импери|религи|философ|наук|технолог|эконом|церк",
    ),
    (
        "adventure",
        r"приключ|выжива|спасени|борьб|геро|квест|интриг|тайн|загад|опасност|расслед|убий|преступ|месть|битв|сражен|боев|поиск|путешеств",
    ),
)


def tag_category(tag: str) -> str:
    for group, pattern in TAG_RULES:
        if re.search(pattern, tag, re.I):
            return group
    return "other"


def book_terms(record: dict) -> list[tuple[str, str, str, str, str]]:
    """kind, stable value, visible label, searchable label, group. Dedup per book."""
    terms = {}

    def add(kind, value, label, group=""):
        label = label.strip()[:500]
        if label:
            terms[(kind, value)] = (kind, value, label, normalized(label), group)

    for author in record.get("authors") or [record.get("author", "")]:
        add("author", normalized(author), author)
    add("series", normalized(record.get("series", "")), record.get("series", ""))
    for term in record.get("genres", []):
        raw = normalized(term)
        code = GENRE_ALIASES.get(raw, raw)
        if code in GENRES:
            group, label = GENRES[code]
            add("genre", code, label, group)
        else:
            add("tag", raw, term, tag_category(term))
    return list(terms.values())


def decorate_record(record: dict) -> dict:
    # Keep the original categories for compatibility; expose clean UI fields.
    result = dict(record)
    terms = book_terms(record)
    for kind, key in (("genre", "genreItems"), ("tag", "tagItems")):
        result[key] = [
            {"value": value, "label": label, "group": group}
            for k, value, label, _, group in terms
            if k == kind
        ]
    return result
