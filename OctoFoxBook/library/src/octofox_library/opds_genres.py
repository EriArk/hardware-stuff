"""Russian navigation labels for the FB2 genres used by the OPDS facade."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Genre:
    code: str
    title: str


@dataclass(frozen=True, slots=True)
class GenreGroup:
    slug: str
    title: str
    genres: tuple[Genre, ...]


GENRE_GROUPS = (
    GenreGroup(
        "fantasy",
        "Фэнтези",
        (
            Genre("foreign_fantasy", "Зарубежное фэнтези"),
            Genre("russian_fantasy", "Русское фэнтези"),
            Genre("historical_fantasy", "Историческое фэнтези"),
            Genre("city_fantasy", "Городское фэнтези"),
            Genre("humor_fantasy", "Юмористическое фэнтези"),
            Genre("fantasy_fight", "Боевое фэнтези"),
            Genre("magician_book", "Книги о магах"),
            Genre("popadanec", "Попаданцы"),
            Genre("love_fantasy", "Любовное фэнтези"),
        ),
    ),
    GenreGroup(
        "science-fiction",
        "Фантастика",
        (
            Genre("sf", "Фантастика"),
            Genre("sf_action", "Боевая фантастика"),
            Genre("sf_heroic", "Героическая фантастика"),
            Genre("sf_history", "Альтернативная история"),
            Genre("sf_detective", "Фантастический детектив"),
            Genre("sf_cyberpunk", "Киберпанк"),
            Genre("sf_space", "Космическая фантастика"),
            Genre("sf_social", "Социальная фантастика"),
            Genre("sf_horror", "Ужасы и мистика"),
            Genre("sf_humor", "Юмористическая фантастика"),
            Genre("sf_fantasy", "Фэнтезийная фантастика"),
            Genre("foreign_sf", "Зарубежная фантастика"),
            Genre("love_sf", "Любовная фантастика"),
        ),
    ),
    GenreGroup(
        "detectives",
        "Детективы и триллеры",
        (
            Genre("detective", "Детективы"),
            Genre("det_classic", "Классические детективы"),
            Genre("det_police", "Полицейские детективы"),
            Genre("det_action", "Боевики"),
            Genre("det_irony", "Иронические детективы"),
            Genre("det_history", "Исторические детективы"),
            Genre("det_espionage", "Шпионские детективы"),
            Genre("det_crime", "Криминальные детективы"),
            Genre("det_political", "Политические детективы"),
            Genre("det_maniac", "Маньяки"),
            Genre("det_hard", "Крутые детективы"),
            Genre("foreign_detective", "Зарубежные детективы"),
            Genre("love_detective", "Любовные детективы"),
            Genre("thriller", "Триллеры"),
        ),
    ),
    GenreGroup(
        "adventures",
        "Приключения",
        (
            Genre("adventure", "Приключения"),
            Genre("adv_western", "Вестерны"),
            Genre("adv_history", "Исторические приключения"),
            Genre("adv_indian", "Приключения об индейцах"),
            Genre("adv_maritime", "Морские приключения"),
            Genre("adv_geo", "Путешествия и география"),
            Genre("adv_animal", "Природа и животные"),
        ),
    ),
    GenreGroup(
        "prose",
        "Проза и классика",
        (
            Genre("prose", "Проза"),
            Genre("prose_classic", "Классическая проза"),
            Genre("prose_history", "Историческая проза"),
            Genre("prose_contemporary", "Современная проза"),
            Genre("prose_counter", "Контркультура"),
            Genre("prose_rus_classic", "Русская классика"),
            Genre("prose_su_classics", "Советская классика"),
            Genre("russian_contemporary", "Современная русская литература"),
            Genre("foreign_contemporary", "Современная зарубежная литература"),
            Genre("foreign_prose", "Зарубежная проза"),
            Genre("literature_19", "Литература XIX века"),
            Genre("literature_20", "Литература XX века"),
        ),
    ),
    GenreGroup(
        "romance",
        "Любовные романы",
        (
            Genre("love_contemporary", "Современные любовные романы"),
            Genre("love_history", "Исторические любовные романы"),
            Genre("love_short", "Короткие любовные романы"),
            Genre("love_erotica", "Эротическая литература"),
        ),
    ),
    GenreGroup(
        "children",
        "Детская литература",
        (
            Genre("children", "Детская литература"),
            Genre("foreign_children", "Зарубежная детская литература"),
            Genre("child_tale", "Сказки"),
            Genre("child_verse", "Детские стихи"),
            Genre("child_prose", "Детская проза"),
            Genre("child_sf", "Детская фантастика"),
            Genre("child_det", "Детские детективы"),
            Genre("child_adv", "Детские приключения"),
            Genre("child_education", "Обучающая литература"),
        ),
    ),
    GenreGroup(
        "poetry-humor",
        "Поэзия, драматургия и юмор",
        (
            Genre("poetry", "Поэзия"),
            Genre("drama", "Драматургия"),
            Genre("humor", "Юмор"),
            Genre("humor_prose", "Юмористическая проза"),
            Genre("humor_verse", "Юмористические стихи"),
            Genre("humor_satire", "Сатира"),
        ),
    ),
    GenreGroup(
        "non-fiction",
        "Документальная литература",
        (
            Genre("nonf_biography", "Биографии и мемуары"),
            Genre("nonf_publicism", "Публицистика"),
            Genre("nonf_criticism", "Критика"),
            Genre("design", "Искусство и дизайн"),
            Genre("military", "Военное дело"),
            Genre("travel", "Путешествия"),
            Genre("popular_science", "Научно-популярная литература"),
        ),
    ),
    GenreGroup(
        "science-education",
        "Наука и образование",
        (
            Genre("science", "Наука"),
            Genre("sci_math", "Математика"),
            Genre("sci_physics", "Физика"),
            Genre("sci_chem", "Химия"),
            Genre("sci_biology", "Биология"),
            Genre("sci_tech", "Техника"),
            Genre("sci_history", "История"),
            Genre("sci_psychology", "Психология"),
            Genre("sci_culture", "Культурология"),
            Genre("sci_religion", "Религиоведение"),
            Genre("sci_philosophy", "Философия"),
            Genre("sci_politics", "Политика"),
            Genre("sci_business", "Экономика"),
            Genre("sci_juris", "Право"),
            Genre("sci_linguistic", "Языкознание"),
            Genre("sci_medicine", "Медицина"),
            Genre("sci_geo", "География"),
            Genre("sci_social_studies", "Обществознание"),
            Genre("textbook", "Учебники"),
            Genre("pedagogy", "Педагогика"),
            Genre("foreign_language", "Иностранные языки"),
            Genre("foreign_edu", "Зарубежная учебная литература"),
        ),
    ),
    GenreGroup(
        "computers",
        "Компьютеры и технологии",
        (
            Genre("computers", "Компьютеры"),
            Genre("comp_www", "Интернет"),
            Genre("comp_programming", "Программирование"),
            Genre("comp_hard", "Компьютерное железо"),
            Genre("comp_soft", "Программы"),
            Genre("comp_db", "Базы данных"),
            Genre("comp_osnet", "Операционные системы и сети"),
            Genre("comp_dsp", "Цифровая обработка сигналов"),
            Genre("comp_all", "О компьютерах"),
        ),
    ),
    GenreGroup(
        "business",
        "Бизнес и экономика",
        (
            Genre("accounting", "Бухгалтерский учёт"),
            Genre("banking", "Банковское дело"),
            Genre("economics", "Экономика"),
            Genre("management", "Управление"),
            Genre("marketing", "Маркетинг"),
            Genre("org_behavior", "Корпоративная культура"),
            Genre("personal_finance", "Личные финансы"),
            Genre("real_estate", "Недвижимость"),
            Genre("popular_business", "Популярно о бизнесе"),
            Genre("small_business", "Малый бизнес"),
            Genre("industries", "Отрасли"),
            Genre("job_hunting_career", "Карьера"),
            Genre("paper_work", "Делопроизводство"),
            Genre("economics_ref", "Экономические справочники"),
            Genre("foreign_business", "Зарубежная деловая литература"),
        ),
    ),
    GenreGroup(
        "reference",
        "Справочная литература",
        (
            Genre("reference", "Справочники"),
            Genre("ref_encyc", "Энциклопедии"),
            Genre("ref_dict", "Словари"),
            Genre("ref_ref", "Справочные издания"),
            Genre("ref_guide", "Руководства"),
        ),
    ),
    GenreGroup(
        "religion",
        "Религия и эзотерика",
        (
            Genre("religion", "Религия"),
            Genre("relig_history", "История религии"),
            Genre("religion_rel", "Религиозная литература"),
            Genre("religion_esoterics", "Эзотерика"),
            Genre("religion_self", "Духовное развитие"),
            Genre("foreign_religion", "Зарубежная религиозная литература"),
        ),
    ),
    GenreGroup(
        "home",
        "Дом, здоровье и спорт",
        (
            Genre("home", "Дом и семья"),
            Genre("home_cooking", "Кулинария"),
            Genre("home_pets", "Домашние животные"),
            Genre("home_crafts", "Рукоделие"),
            Genre("home_entertain", "Развлечения"),
            Genre("home_health", "Здоровье"),
            Genre("home_garden", "Сад и огород"),
            Genre("home_diy", "Сделай сам"),
            Genre("home_sport", "Спорт"),
            Genre("home_sex", "Отношения"),
        ),
    ),
)

GENRES_BY_CODE = {
    genre.code.casefold(): (group, genre)
    for group in GENRE_GROUPS
    for genre in group.genres
}
GROUPS_BY_SLUG = {group.slug: group for group in GENRE_GROUPS}
