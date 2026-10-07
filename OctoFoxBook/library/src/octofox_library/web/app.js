"use strict";
const $ = (id) => document.getElementById(id);
const esc = (text) =>
  String(text ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const icon = (name) => `<svg aria-hidden="true"><use href="#i-${name}"/></svg>`;
const views = {
  personal: "Моя библиотека",
  library: "Все книги",
  recent: "Новинки",
  collections: "Подборки",
  "collection-korean-elves": "Корейские эльфы",
  reading: "Читаю сейчас",
  want: "Хочу прочитать",
  favorite: "Избранное",
  read: "Прочитанные",
  queue: "Моя читалка",
  genres: "Жанры",
  tags: "Теги",
  authors: "Авторы",
  series: "Серии",
};
const directoryKinds = {
  genres: "genre",
  tags: "tag",
  authors: "author",
  series: "series",
};
const directoryViews = Object.fromEntries(
  Object.entries(directoryKinds).map(([v, k]) => [k, v]),
);
const state = {
  csrf: "",
  user: "",
  view: "library",
  page: 1,
  devices: [],
  book: null,
  request: 0,
  reader: null,
  queueTimer: null,
  queueRequest: 0,
  catalogTimer: null,
  catalogRevision: 0,
  filters: {},
  personalCollection: '',
  browse: {
    kind: "genre",
    page: 1,
    letter: "",
    request: 0,
    returnView: "library",
    returnContext: null,
    combineFilters: false,
  },
};
let toastTimer, searchTimer, positionTimer, browseTimer;
let releaseBookDownload = () => {};
let readerVoice = null;
let serverSpeechInfo = {available: true, voice: 'eugene', voices: [
  {voiceURI: 'eugene', name: 'Евгений · Silero', lang: 'ru-RU'},
  {voiceURI: 'ruslan', name: 'Руслан · Piper', lang: 'ru-RU'},
]};
const readerScreenAwake = createReaderScreenAwake(navigator, document, window);
const personalLibrary = window.BookPersonal?.createPersonalLibrary({api, session: () => state.csrf,
  onFilter: value => { state.personalCollection = value; if (state.view === 'personal') loadCatalog().catch(e => toast(e.message)); },
  onChange: () => refreshPersonalLibrary(),
});
function refreshPersonalLibrary() {
  if (state.view !== 'personal') return;
  personalLibrary?.refresh().catch(e => toast(e.message));
  loadCatalog().catch(e => toast(e.message));
}
async function api(path, data, options = {}) {
  const response = await fetch(`/reader-api${path}`, {
    credentials: "same-origin",
    ...options,
    ...(data === undefined
      ? {}
      : {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "X-CSRF-Token": state.csrf,
          },
          body: JSON.stringify(data),
        }),
  });
  const value = await response.json();
  if (!response.ok) {
    if (response.status === 401 && path !== "/login") showLogin();
    const error = new Error(value.error || "Не удалось выполнить запрос");
    error.status = response.status;
    error.retryAfter = Number(response.headers?.get('Retry-After')) || 60;
    throw error;
  }
  return value;
}
function toast(message) {
  $("toast").textContent = message;
  $("toast").hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => ($("toast").hidden = true), 4000);
}
function task(fn) {
  return (event) => Promise.resolve(fn(event)).catch((e) => toast(e.message));
}
function showLogin() {
  readerScreenAwake.setActive(false);
  closePersonalSort();
  $("uploadDialog").close();
  personalLibrary?.reset();
  if (activeUpload) activeUpload.cancelled = true;
  $("uploadErrors").hidden = true;
  $("uploadErrorsList").replaceChildren();
  $("bookmarksDialog")?.close();
  readerVoice?.stop();
  setVoicePopover(false, false);
  state.reader?.bookPages?.close();
  state.reader = null;
  clearInterval(state.queueTimer);
  clearInterval(state.catalogTimer);
  state.request++;
  state.browse.request++;
  clearTimeout(browseTimer);
  $("appScreen").hidden = true;
  $("loginScreen").hidden = false;
  $("readerScreen").hidden = true;
  if ($("bookDialog").open) $("bookDialog").close();
  document.body.classList.remove("locked");
  setDrawer(false);
  state.csrf = "";
  state.book = null;
  state.reader = null;
  state.filters = {};
}
async function enter(me) {
  state.csrf = me.csrf;
  state.user = me.username;
  state.devices = me.devices || (await api("/devices"));
  $("accountName").textContent = state.user;
  $("loginScreen").hidden = true;
  $("appScreen").hidden = false;
  $("password").value = "";
  await navigate(
    views[location.hash.slice(1)] ? location.hash.slice(1) : "library",
  );
  state.queueTimer = setInterval(() => {
    if (document.hidden) return;
    if (state.view === "queue") loadQueue().catch(() => {});
    else refreshDot().catch(() => {});
  }, 20000);
  refreshDot().catch(() => {});
  clearInterval(state.catalogTimer);
  state.catalogTimer = setInterval(() => checkCatalogUpdates().catch(() => {}), 60000);
}
$("loginForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const button = event.submitter;
  button.disabled = true;
  button.textContent = "Открываем…";
  $("loginError").textContent = "";
  try {
    const me = await api("/login", {
      username: $("username").value.trim(),
      password: $("password").value,
    });
    await enter(me);
  } catch (e) {
    $("loginError").textContent = e.message;
  } finally {
    button.disabled = false;
    button.innerHTML = "Открыть библиотеку <span>↗</span>";
  }
});
$("logoutButton").onclick = task(async () => {
  await savePosition();
  await api("/logout", {});
  showLogin();
});

function setDrawer(open) {
  const pinned = window.matchMedia?.('(min-width: 768px) and (min-height: 600px)').matches;
  if (pinned) open = false;
  const drawer = $("drawer");
  drawer.inert = !pinned && !open;
  drawer.classList.toggle("open", open);
  $("drawerBackdrop").hidden = !open;
  $("menuButton").setAttribute("aria-expanded", String(open));
  document.body.classList.toggle(
    "locked",
    open || $("bookDialog").open || !$("readerScreen").hidden,
  );
  if (open) $("closeMenu").focus();
  else if (!pinned && drawer.contains(document.activeElement)) $("menuButton").focus();
}
window.matchMedia?.('(min-width: 768px) and (min-height: 600px)').addEventListener('change', () => setDrawer(false));
setDrawer(false);
$("menuButton").onclick = () => setDrawer(true);
$("closeMenu").onclick = () => setDrawer(false);
$("drawerBackdrop").onclick = () => setDrawer(false);
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    if ($("bookmarksDialog")?.open) { event.preventDefault(); $("bookmarksDialog").close(); return; }
    if (state.reader && !$("voiceControls").hidden) {
      event.preventDefault();
      setVoicePopover(false);
      return;
    }
    setDrawer(false);
    if (state.reader) closeReader().catch((e) => toast(e.message));
  }
  if (event.key === "Tab" && $("drawer").classList.contains("open")) {
    const nodes = [...$("drawer").querySelectorAll("button")];
    const first = nodes[0],
      last = nodes.at(-1);
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  }
});
let touch = null,
  lastSwipe = 0;
document.addEventListener(
  "click",
  (event) => {
    if (Date.now() - lastSwipe < 350) {
      event.preventDefault();
      event.stopImmediatePropagation();
    }
  },
  true,
);
document.addEventListener(
  "touchstart",
  (event) => {
    if (event.touches.length !== 1) return;
    const t = event.touches[0];
    touch = {
      x: t.clientX,
      y: t.clientY,
      target: event.target,
      time: Date.now(),
    };
  },
  { passive: true },
);
document.addEventListener(
  "touchend",
  (event) => {
    if (!touch) return;
    const t = event.changedTouches[0],
      dx = t.clientX - touch.x,
      dy = t.clientY - touch.y;
    const start = touch;
    touch = null;
    if (
      Math.abs(dx) < 75 ||
      Math.abs(dy) > 50 ||
      Date.now() - start.time > 700 ||
      start.target.closest("input,select,dialog")
    )
      return;
    if (state.reader || !state.csrf) return;
    if ($("drawer").classList.contains("open")) {
      if (dx < 0) {
        lastSwipe = Date.now();
        setDrawer(false);
      }
    } else if (dx > 0) {
      lastSwipe = Date.now();
      setDrawer(true);
    }
  },
  { passive: true },
);
$("navigation").onclick = task(async (event) => {
  const target = event.target.closest("[data-view]");
  if (target) {
    state.browse.returnView = "library";
    await navigate(target.dataset.view);
  }
});
$("queueShortcut").onclick = task(() => navigate("queue"));
window.addEventListener(
  "hashchange",
  task(() => {
    const view = location.hash.slice(1);
    if (views[view] && state.csrf && view !== state.view) return navigate(view);
  }),
);
function catalogContext() {
  return {
    view: state.view,
    personalCollection: state.personalCollection,
    filters: { ...state.filters },
    q: $("searchInput").value,
    scope: $("searchScope").value,
    sort: $("sortFilter").value,
  };
}
async function navigate(view, context = {}) {
  closePersonalSort();
  $("personalSortButton").hidden = view !== 'personal';
  $("uploadDialog").close();
  state.request++;
  state.browse.request++;
  clearTimeout(searchTimer);
  clearTimeout(browseTimer);
  // A destination owns its query. Only explicit picker/Back contexts may carry it.
  state.filters = { ...context.filters };
  $("searchInput").value = context.q || "";
  $("searchScope").value = context.scope || "all";
  $("sortFilter").value = context.sort || "recent";
  state.page = 1;
  $("filters").hidden = true;
  $("filterButton").setAttribute("aria-expanded", "false");
  state.view = view;
  state.personalCollection = context.personalCollection || '';
  $("uploadControls").hidden = view !== "personal";
  location.hash = view;
  setDrawer(false);
  document
    .querySelectorAll("[data-view]")
    .forEach((e) => e.classList.toggle("active", e.dataset.view === (view.startsWith("collection-") ? "collections" : view)));
  $("libraryScreen").hidden = ["queue", "collections"].includes(view) || !!directoryKinds[view];
  $("collectionsScreen").hidden = view !== "collections";
  $("collectionBack").hidden = !view.startsWith("collection-");
  $("browseScreen").hidden = !directoryKinds[view];
  $("queueScreen").hidden = view !== "queue";
  if (view === "collections") {
    await loadCollections();
    return;
  }
  if (directoryKinds[view]) {
    state.browse.returnContext = context.returnContext || null;
    state.browse.returnView = context.returnContext?.view || "library";
    state.browse.combineFilters = context.combineFilters === true;
    state.browse.kind = directoryKinds[view];
    state.browse.letter = "";
    $("browseSearch").value = "";
    $("browseGroup").value = "";
    $("browseSort").value = "alpha";
    $("browseTitle").textContent = views[view];
    $("browseSearch").placeholder = {
      genre: "Найти жанр",
      tag: "Найти тег",
      author: "Имя или фамилия автора",
      series: "Название серии",
    }[state.browse.kind];
    $("browseSearch").setAttribute("aria-label", $("browseSearch").placeholder);
    $("browseGroupLabel").hidden = !["genre", "tag"].includes(
      state.browse.kind,
    );
    $("browseHint").textContent = "";
    $("browseKinds")
      .querySelectorAll("button")
      .forEach((b) =>
        b.setAttribute(
          "aria-pressed",
          String(b.dataset.kind === state.browse.kind),
        ),
      );
    await loadDirectory();
    return;
  }
  if (view === "queue") {
    await loadQueue();
    return;
  }
  $("viewTitle").textContent = views[view];
  $("sectionTitle").textContent =
    view.startsWith("collection-") ? "В подборке"
      : view === "personal"
      ? "Сохранённые книги"
      : view === "reading" ? "История чтения"
      : view === "recent"
        ? "Новое в архиве"
        : "На книжных полках";
  $("viewDescription").textContent =
    view === "personal"
      ? "Книги, которые ты сохранил себе."
      : view === "reading" ? "Последняя открытая книга — наверху."
      : view === "recent" ? "Последние 100 добавленных книг"
      : "Читай здесь или отправь в читалку.";
  $("viewDescription").hidden = view !== "recent";
  $("sortFilter").querySelector('[value="recent"]').textContent =
    view === "personal" ? "Недавно сохранённые"
      : view === "reading" ? "Недавно читали" : "Сначала новые";
  if (["recent", "reading"].includes(view)) $("sortFilter").value = "recent";
  $("sortFilter").closest("label").hidden = ["recent", "reading", "personal"].includes(view);
  if (view === 'personal') await personalLibrary?.enter(state.personalCollection);
  if (view !== state.view) return;
  await loadCatalog();
}
function parameters(page) {
  const query = new URLSearchParams({
    page: String(page),
    sort: $("sortFilter").value,
    scope: $("searchScope").value,
  });
  for (const [key, id] of [["q", "searchInput"]]) {
    const v = $(id).value.trim();
    if (v) query.set(key, v);
  }
  for (const [key, item] of Object.entries(state.filters))
    query.set(key, item.value);
  if (state.view.startsWith("collection-")) query.set("collection", state.view.slice("collection-".length));
  else if (state.view === "personal") {
    query.set("personal", "1");
    if (state.personalCollection) query.set('personalCollection', state.personalCollection);
  }
  else if (state.view === "recent") query.set("new", "1");
  else if (!["library", "recent"].includes(state.view))
    query.set("shelf", state.view);
  return query;
}
async function loadCollections() {
  const request = ++state.request, account = state.csrf;
  $("collectionCards").replaceChildren();
  $("collectionsStatus").textContent = "Собираем истории…";
  $("collectionsRefresh").disabled = true;
  try {
    const result = await api("/collections");
    if (request !== state.request || account !== state.csrf || state.view !== "collections") return;
    state.catalogRevision = result.revision;
    $("collectionCards").innerHTML = result.collections.map(item =>
      `<a class="collection-card" href="#collection-${esc(item.id)}" aria-label="${esc(item.title)} — ${item.count} книг">`
      + `<img src="${esc(item.image)}" width="1774" height="887" alt="" decoding="async">`
      + `<div class="collection-caption"><span class="collection-kicker">Подборка · ${item.count} книг</span>`
      + `<h2>${esc(item.title)}</h2><span class="collection-description">${esc(item.description)}</span></div>`
      + `<span class="collection-arrow" aria-hidden="true">↗</span></a>`).join("");
    $("collectionsStatus").textContent = "";
    renderCatalogStatus(result.status, result.revision);
  } catch (error) {
    if (request === state.request && account === state.csrf)
      $("collectionsStatus").textContent = error.message;
  } finally {
    if (request === state.request) $("collectionsRefresh").disabled = false;
  }
}
function cover(book, lazy = true) {
  const fallback = fallbackCover(book);
  return `<div class="cover-wrap"><span class="cover-fallback" aria-hidden="true" data-fallback-cover="${fallback}"${book.cover ? '' : ` style="background-image:url('${fallback}')"`}><span class="cover-fallback-content">${icon('book')}<span class="cover-fallback-title">${esc(book.title || 'Без названия')}</span></span></span>${book.cover ? `<img ${book.coverDeferred ? "data-cover-src" : "src"}="${esc(book.cover)}" width="240" height="360" alt="Обложка: ${esc(book.title)}" loading="${lazy ? "lazy" : "eager"}" decoding="async">` : ""}</div>`;
}
function fallbackCover(book) {
  // Stable pseudo-random art: no account state, browser storage or reshuffling.
  const identity = String(book.id || book.acquisition || `${book.title || ''}|${book.author || book.authors?.join(', ') || ''}`).normalize('NFC');
  let hash = 2166136261;
  for (let i = 0; i < identity.length; i++) hash = Math.imul(hash ^ identity.charCodeAt(i), 16777619);
  return `/cover-art/v1/${String((hash >>> 0) % 16 + 1).padStart(2, '0')}.webp`;
}
function bindCovers(root) {
  for (const img of root.querySelectorAll("img")) {
    const ready = () => {
      img.classList.add("loaded");
      img.previousElementSibling.hidden = true;
    };
    img.onload = ready;
    img.onerror = () => {
      const fallback = img.previousElementSibling;
      if (fallback?.dataset?.fallbackCover) {
        fallback.style.backgroundImage = `url('${fallback.dataset.fallbackCover}')`;
        fallback.hidden = false;
      }
      img.remove();
    };
    if (img.complete && img.naturalWidth) ready();
  }
}
function hasReadingPosition(book) {
  const reading = book.reading;
  return !!reading && (reading.chapter > 0 || reading.offset > 0 || reading.shelf === "reading");
}
function readingTileClass(view, append, index) {
  return view === "reading" && !append && index === 0 ? "book-tile continue-tile" : "book-tile";
}
async function loadCatalog(append = false) {
  const request = ++state.request,
    page = append ? state.page + 1 : 1;
  $("catalogStatus").textContent = append
    ? "Загружаем ещё книги…"
    : "Открываем полку…";
  $("loadMore").disabled = true;
  renderFilters();
  if (!append) $("bookGrid").replaceChildren();
  try {
    const result = await api(`/books?${parameters(page)}`);
    if (request !== state.request) return;
    state.catalogRevision = result.catalogRevision;
    $("catalogUpdates").hidden = true;
    state.page = page;
    $("bookCount").textContent = result.total.toLocaleString("ru");
    $("resultCount").textContent = `${result.total.toLocaleString("ru")} книг`;
    renderFilters();
    const fragment = document.createDocumentFragment();
    for (const book of result.books) {
      const tile = document.createElement("button");
      tile.className = readingTileClass(state.view, append, fragment.childElementCount);
      tile.setAttribute(
        "aria-label",
        `${book.title}, ${book.author}. Открыть карточку`,
      );
      const caption = `<h3>${esc(book.title)}</h3><p>${esc(book.author || "Автор не указан")}</p>`;
      const continuing = tile.classList.contains("continue-tile");
      tile.innerHTML = `${cover(book, append || fragment.childElementCount >= 6)}${continuing ? caption + (book.series ? `<p class="series">${esc(book.series)}</p>` : "") : `<div class="tile-caption">${caption}</div>`}`;
      if (hasReadingPosition(book) && continuing)
        tile.insertAdjacentHTML("beforeend", `<p class="reading-position">Продолжить · раздел ${Number(book.reading.chapter) + 1}</p>`);
      else if (hasReadingPosition(book))
        tile.insertAdjacentHTML("beforeend", `<span class="tile-badge">Раздел ${Number(book.reading.chapter) + 1}</span>`);
      if (continuing)
        tile.insertAdjacentHTML("afterbegin", '<span class="continue-kicker">Читали последней</span>');
      tile.onclick = task(() => openBook(book.id));
      fragment.append(tile);
    }
    $("bookGrid").append(fragment);
    bindCovers($("bookGrid"));
    $("loadMore").hidden = !result.more;
    const loading = result.indexStatus === "loading";
    renderCatalogStatus(result.indexStatus, result.catalogRevision);
    if (!result.books.length && !append && !loading)
      $("bookGrid").innerHTML =
        state.view === "personal" &&
        !$("searchInput").value.trim() &&
        !Object.keys(state.filters).length
          ? '<div class="empty-state">Здесь будут твои книги.<br>Открой книгу в каталоге и нажми «В мою библиотеку».<br><a href="#library" class="text-button">Перейти в каталог →</a></div>'
          : '<div class="empty-state">На этой полке пока пусто.<br>Попробуй изменить фильтры или выбрать другую полку.</div>';
  } catch (e) {
    if (request !== state.request) return;
    $("catalogStatus").textContent = e.message;
    throw e;
  } finally {
    if (request === state.request) $("loadMore").disabled = false;
  }
}
function renderFilters() {
  const labels = {
    author: "Автор",
    series: "Серия",
    genre: "Жанр",
    tag: "Тег",
    genreGroup: "Жанры",
    tagGroup: "Теги",
  };
  $("activeFilters").innerHTML = Object.entries(state.filters)
    .map(
      ([key, item]) =>
        `<button class="chip" data-remove="${key}" aria-label="Убрать фильтр: ${esc(item.label)}">${labels[key]}: ${esc(item.label)} ×</button>`,
    )
    .join("");
  const options = {scope: $("searchScope").value, sort: $("sortFilter").value};
  const optionNames = {scope: {title: "По названию", author: "По автору", series: "По серии", genre: "По жанру", tag: "По тегу"},
    sort: {title: "Названия А–Я", title_desc: "Названия Я–А", author: "Авторы А–Я", series: "По порядку серии"}};
  for (const [key, value] of Object.entries(options))
    if (optionNames[key][value]) $("activeFilters").insertAdjacentHTML("beforeend",
      `<button class="chip" data-reset-option="${key}" aria-label="Сбросить: ${optionNames[key][value]}">${optionNames[key][value]} ×</button>`);
  const active = Object.keys(state.filters).length + Number(options.scope !== "all") + Number(options.sort !== "recent");
  $("filterButton").setAttribute("data-active", active ? String(active) : "");
  $("filterButton").setAttribute("aria-label", active ? `Фильтры: ${active}` : "Фильтры");
  for (const kind of ["author", "series", "genre", "tag"])
    $(kind + "Choice").textContent =
      state.filters[kind]?.label ||
      state.filters[kind + "Group"]?.label ||
      "Выбрать →";
}
$("activeFilters").onclick = task((event) => {
  const option = event.target.closest("[data-reset-option]");
  if (option) {
    const scope = option.dataset.resetOption === "scope";
    $(scope ? "searchScope" : "sortFilter").value = scope ? "all" : "recent";
    return loadCatalog();
  }
  const button = event.target.closest("[data-remove]");
  if (!button) return;
  delete state.filters[button.dataset.remove];
  return loadCatalog();
});
document.querySelectorAll("[data-browse]").forEach(
  (button) =>
    (button.onclick = task(() => {
      return navigate(directoryViews[button.dataset.browse], {
        returnContext: catalogContext(),
        combineFilters: !!button.closest("#filters"),
      });
    })),
);
$("browseKinds").onclick = task((event) => {
  const button = event.target.closest("[data-kind]");
  if (button) return navigate(directoryViews[button.dataset.kind], {
    returnContext: state.browse.returnContext,
    combineFilters: state.browse.combineFilters,
  });
});
$("backToBooks").onclick = task(() => navigate(
  state.browse.returnView, state.browse.returnContext || {},
));
async function chooseFacet(kind, item, origin = state.browse.returnContext,
                           combine = state.browse.combineFilters) {
  const filters = combine ? { ...origin?.filters } : {};
  if (kind.endsWith("Group")) delete filters[kind.replace("Group", "")];
  else delete filters[kind + "Group"];
  filters[kind] = { value: item.value, label: item.label };
  const view = origin?.view || "library";
  await navigate(directoryKinds[view] || view === "queue" ? "library" : view, {
    filters,
    personalCollection: origin?.personalCollection || '',
    q: combine ? origin?.q : "",
    scope: combine ? origin?.scope : "all",
    sort: kind === "series" ? "series" : combine ? origin?.sort : "recent",
  });
  window.scrollTo(0, 0);
}
async function loadDirectory(append = false) {
  const browse = state.browse,
    request = ++browse.request;
  const page = append ? browse.page + 1 : 1;
  const group = $("browseGroup").value;
  const query = new URLSearchParams({
    kind: browse.kind,
    q: $("browseSearch").value.trim(),
    group,
    letter: browse.letter,
    sort: $("browseSort").value,
    page: String(page),
  });
  $("browseStatus").textContent = "Ищем…";
  $("browseMore").disabled = true;
  if (!append) $("browseResults").replaceChildren();
  try {
    const result = await api(`/facets?${query}`);
    if (request !== browse.request) return;
    browse.page = page;
    $("browseGroup").innerHTML =
      '<option value="">Все категории</option>' +
      result.groups
        .map(
          (g) =>
            `<option value="${esc(g.value)}">${esc(g.label)} · ${g.count.toLocaleString("ru")}</option>`,
        )
        .join("");
    $("browseGroup").value = group;
    $("browseGroups").hidden =
      !!group || !!query.get("q") || !!browse.letter || !result.groups.length;
    $("browseGroups").innerHTML = result.groups
      .map(
        (g) =>
          `<button data-group="${esc(g.value)}"><strong>${esc(g.label)}</strong><span>${g.count.toLocaleString("ru")} книг · ${g.terms} ${browse.kind === "tag" ? "тегов" : "жанров"}</span></button>`,
      )
      .join("");
    const letterScroll = $("browseLetters").scrollLeft;
    $("browseLetters").innerHTML = ["", ...result.letters]
      .map(
        (letter) =>
          `<button data-letter="${esc(letter)}" aria-pressed="${letter === browse.letter}">${esc(letter || "Все")}</button>`,
      )
      .join("");
    $("browseLetters").scrollLeft = letterScroll;
    const currentGroup = result.groups.find((g) => g.value === group);
    $("chooseGroup").hidden = !currentGroup;
    $("chooseGroup").textContent = currentGroup
      ? `Все книги: ${currentGroup.label} →`
      : "";
    $("chooseGroup").onclick = task(() =>
      chooseFacet(browse.kind + "Group", currentGroup),
    );
    $("browseListTitle").textContent =
      currentGroup?.label || views[directoryViews[browse.kind]];
    $("browseCount").textContent =
      `${result.total.toLocaleString("ru")} найдено`;
    const fragment = document.createDocumentFragment();
    for (const item of result.items) {
      const button = document.createElement("button");
      button.className = "directory-item";
      button.innerHTML = `<span>${esc(item.label)}</span><small>${item.count.toLocaleString("ru")} книг →</small>`;
      button.onclick = task(() => chooseFacet(browse.kind, item));
      fragment.append(button);
    }
    $("browseResults").append(fragment);
    $("browseMore").hidden = !result.more;
    $("browseStatus").textContent = result.total
      ? ""
      : "Ничего не найдено. Измени запрос, букву или категорию.";
  } catch (e) {
    if (request === browse.request) $("browseStatus").textContent = e.message;
  } finally {
    if (request === browse.request) $("browseMore").disabled = false;
  }
}
$("browseGroups").onclick = task((event) => {
  const button = event.target.closest("[data-group]");
  if (!button) return;
  $("browseGroup").value = button.dataset.group;
  state.browse.letter = "";
  return loadDirectory();
});
$("browseLetters").onclick = task((event) => {
  const button = event.target.closest("[data-letter]");
  if (!button) return;
  state.browse.letter = button.dataset.letter;
  return loadDirectory();
});
$("browseSearch").oninput = () => {
  clearTimeout(browseTimer);
  state.browse.request++;
  state.browse.letter = "";
  browseTimer = setTimeout(() => loadDirectory(), 300);
};
$("browseSearchForm").onsubmit = task((event) => {
  event.preventDefault();
  clearTimeout(browseTimer);
  return loadDirectory();
});
for (const id of ["browseGroup", "browseSort"])
  $(id).onchange = task(() => {
    state.browse.letter = "";
    return loadDirectory();
  });
$("browseMore").onclick = task(() => loadDirectory(true));
$("searchForm").onsubmit = task((event) => {
  event.preventDefault();
  clearTimeout(searchTimer);
  return loadCatalog();
});
$("searchInput").oninput = () => {
  clearTimeout(searchTimer);
  state.request++;
  searchTimer = setTimeout(
    () => loadCatalog().catch((e) => toast(e.message)),
    400,
  );
};
$("filterButton").onclick = () => {
  const open = $("filters").hidden;
  $("filters").hidden = !open;
  $("filterButton").setAttribute("aria-expanded", String(open));
};
function closePersonalSort(focus = false) {
  const dialog = $("personalSortDialog");
  if (!dialog.open) return;
  dialog.close();
  $("personalSortButton").setAttribute('aria-expanded', 'false');
  if (focus) $("personalSortButton").focus();
}
function positionPersonalSort() {
  const dialog = $("personalSortDialog");
  if (!dialog.open) return;
  const rect = $("personalSortButton").getBoundingClientRect();
  const width = dialog.offsetWidth, height = window.innerHeight;
  dialog.style.left = `${Math.max(12, Math.min(rect.right - width, window.innerWidth - width - 12))}px`;
  const top = Math.max(12, Math.min(rect.bottom + 8, height - dialog.offsetHeight - 12));
  dialog.style.top = `${top}px`;
  dialog.style.maxHeight = `${Math.max(44, height - top - 12)}px`;
}
$("personalSortButton").onclick = () => {
  const dialog = $("personalSortDialog");
  if (dialog.open) return closePersonalSort();
  if (state.view !== 'personal') return;
  for (const input of dialog.querySelectorAll('input')) input.checked = input.value === $("sortFilter").value;
  dialog.show();
  $("personalSortButton").setAttribute('aria-expanded', 'true');
  positionPersonalSort();
  dialog.querySelector('input:checked')?.focus();
};
$("closePersonalSort").onclick = () => closePersonalSort(true);
$("personalSortDialog").onchange = task(async event => {
  const value = event.target.value;
  if (!['recent', 'title', 'title_desc', 'author', 'series'].includes(value) || state.view !== 'personal') return;
  const changed = $("sortFilter").value !== value;
  $("sortFilter").value = value;
  closePersonalSort(true);
  if (changed) await loadCatalog();
});
document.addEventListener('pointerdown', event => {
  if (!$("personalSortDialog").contains(event.target) && !$("personalSortButton").contains(event.target)) closePersonalSort();
});
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && $("personalSortDialog").open) {event.preventDefault(); closePersonalSort(true);}
});
document.addEventListener('focusin', event => {
  if (!$("personalSortDialog").contains(event.target) && !$("personalSortButton").contains(event.target)) closePersonalSort();
});
window.addEventListener('resize', positionPersonalSort);
window.addEventListener('scroll', () => closePersonalSort(), {passive: true});
for (const id of ["searchScope", "sortFilter"])
  $(id).onchange = task(() => loadCatalog());
$("resetFilters").onclick = task(() => {
  state.filters = {};
  $("searchInput").value = "";
  $("searchScope").value = "all";
  $("sortFilter").value = "recent";
  return loadCatalog();
});
$("loadMore").onclick = task(() => loadCatalog(true));
$("collectionsRefresh").onclick = task(() => loadCollections());

async function openBook(id) {
  state.book = await api(`/books/${id}`);
  state.devices = await api("/devices");
  renderBook();
  $("bookDialog").showModal();
  document.body.classList.add("locked");
  $("bookDialog").scrollTop = 0;
}
function standaloneApp() {
  return navigator.standalone === true ||
    window.matchMedia("(display-mode: standalone)").matches;
}
function downloadFilename(disposition, title) {
  let name = title;
  const encoded = disposition?.match(/filename\*=UTF-8''([^;]+)/i);
  if (encoded) {
    try { name = decodeURIComponent(encoded[1]); } catch {}
  }
  name = String(name || "Книга").replace(/[\x00-\x1f\x7f/\\:*?"<>|]/g, "_")
    .replace(/\.fb2$/i, "").trim().slice(0, 120);
  return `${name || "Книга"}.fb2`;
}
function bindBookDownload(link, book, status) {
  let file = null, busy = false, disposed = false, controller = null;
  const initialLabel = link.textContent;
  const say = (message) => {
    if (disposed) return;
    status.hidden = false;
    status.textContent = message;
  };
  link.onclick = async (event) => {
    if (!standaloneApp()) return; // Ordinary browser download keeps its own navigation.
    // Never navigate an installed app to a file viewer (Safari bug 236943).
    event.preventDefault();
    if (busy || disposed) return;
    if (!navigator.share || !navigator.canShare) {
      say("Здесь недоступно сохранение файлов. Открой эту библиотеку в Safari и скачай книгу там.");
      return;
    }
    busy = true;
    link.setAttribute("aria-disabled", "true");
    link.setAttribute("aria-busy", "true");
    let timer;
    try {
      if (file) {
        // A separate tap gives Safari fresh user activation, even after a slow download.
        await navigator.share({ files: [file] });
        say("Меню закрыто. Можно продолжать пользоваться библиотекой.");
      } else {
        link.textContent = "Подготовка файла…";
        say("Загружаем книгу. Карточку можно закрыть — загрузка отменится.");
        controller = new AbortController();
        timer = setTimeout(() => controller.abort(), 120000);
        const response = await fetch(link.href, {
          credentials: "same-origin", redirect: "error", signal: controller.signal,
        });
        if (!response.ok) {
          if (response.status === 401) {
            showLogin();
            return;
          }
          const error = await response.json().catch(() => ({}));
          throw new Error(error.error || "Не удалось загрузить файл. Попробуй ещё раз.");
        }
        const maxBytes = 128 * 1024 * 1024;
        if (Number(response.headers.get("Content-Length")) > maxBytes)
          throw new Error("Файл слишком большой для сохранения здесь.");
        const blob = await response.blob();
        if (disposed) return;
        if (!blob.size || blob.size > maxBytes)
          throw new Error("Не удалось подготовить файл книги.");
        const name = downloadFilename(response.headers.get("Content-Disposition"), book.title);
        // Some share targets recognize FB2 by extension but not by its MIME type.
        for (const type of ["application/x-fictionbook+xml", "application/octet-stream"]) {
          const candidate = new File([blob], name, { type });
          if (navigator.canShare({ files: [candidate] })) {
            file = candidate;
            break;
          }
        }
        if (!file) {
          say("iOS не разрешает передать этот формат из веб-приложения. Скачай его с эту библиотеку в Safari.");
          return;
        }
        say("Файл готов. Нажми «Сохранить / открыть…» и выбери «Сохранить в Файлы» или читалку.");
      }
    } catch (error) {
      if (disposed) return;
      if (error.name === "AbortError") {
        say(file ? "Сохранение отменено. Файл готов, можно попробовать снова."
          : "Загрузка заняла слишком много времени. Попробуй ещё раз.");
      } else {
        say(file ? "Не удалось открыть меню сохранения. Нажми «Сохранить / открыть…» ещё раз."
          : error.message || "Не удалось загрузить файл. Попробуй ещё раз.");
      }
    } finally {
      clearTimeout(timer);
      controller?.abort();
      controller = null;
      busy = false;
      if (!disposed) {
        link.textContent = file ? "Сохранить / открыть…" : initialLabel;
        link.removeAttribute("aria-disabled");
        link.removeAttribute("aria-busy");
      }
    }
  };
  return () => {
    disposed = true;
    controller?.abort();
    file = null;
  };
}
function renderBook() {
  releaseBookDownload();
  const b = state.book;
  $("bookDetails").innerHTML =
    `<div class="detail-cover">${cover(b, false)}</div><h2 class="detail-title">${esc(b.title)}</h2><p class="detail-author">${esc(b.author)}</p><div class="detail-actions"><button id="readBook" class="primary">${icon("book")} Читать здесь</button><label class="device-select">Отправить на устройство<select id="targetDevice">${state.devices.map((d) => `<option value="${esc(d.id)}">${esc(d.name)}</option>`).join("")}</select></label><button id="sendBook" class="secondary">${icon("device")} Загрузить на устройство</button><p class="fine">Нажми «Синхронизировать» на главной читалки, чтобы забрать книгу.</p></div><p class="eyebrow">Об этой истории</p><div class="detail-summary">${esc(b.summary || "Аннотация пока не добавлена.")}</div><div class="shelf-row">${[
      ["want", "Хочу прочитать"],
      ["read", "Прочитано"],
    ]
      .map(
        ([key, label]) =>
          `<button data-shelf="${key}" class="${b.reading.shelf === key ? "active" : ""}">${label}</button>`,
      )
      .join("")}</div><div class="detail-meta">${bookMetadata(b)}</div>`;
  bindCovers($("bookDetails"));
  const libraryActions = document.createElement("div");
  libraryActions.className = "library-actions";
  libraryActions.innerHTML = `<button id="saveBook" class="secondary" aria-pressed="${!!b.inLibrary}">${b.inLibrary ? "Убрать из моей библиотеки" : "+ В мою библиотеку"}</button><button id="favoriteBook" class="secondary" aria-pressed="${!!b.isFavorite}">${b.isFavorite ? "♥ Убрать из избранного" : "♡ В избранное"}</button><a id="downloadBook" class="secondary" href="/reader-api/books/${encodeURIComponent(b.id)}/download" download target="_blank" rel="noopener" aria-describedby="downloadStatus">↓ Скачать файл · FB2</a><p id="downloadStatus" class="download-status" role="status" hidden></p>`;
  $("bookDetails").querySelector(".detail-actions").prepend(libraryActions);
  personalLibrary?.attachBook(libraryActions, b, result => {
    $("saveBook").textContent = result.inLibrary ? 'Убрать из моей библиотеки' : '+ В мою библиотеку';
    $("saveBook").setAttribute('aria-pressed', String(result.inLibrary));
  });
  releaseBookDownload = bindBookDownload($("downloadBook"), b, $("downloadStatus"));
  $("readBook").innerHTML = `${icon("book")} ${hasReadingPosition(b) ? "Продолжить чтение" : "Читать здесь"}`;
  $("favoriteBook").onclick = task(async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    try {
      const result = await api(`/books/${b.id}/favorite`, { favorite: !b.isFavorite });
      b.isFavorite = result.isFavorite;
      button.textContent = b.isFavorite ? "♥ Убрать из избранного" : "♡ В избранное";
      button.setAttribute("aria-pressed", String(b.isFavorite));
      if (state.view === "favorite") await loadCatalog();
    } finally {
      button.disabled = false;
    }
  });
  $("saveBook").title = b.inLibrary
    ? "Убрать из моей библиотеки"
    : "Сохранить книгу в мою библиотеку";
  $("saveBook").onclick = task(async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    try {
      const result = await api(`/books/${b.id}/library`, {
        saved: !b.inLibrary,
      });
      b.inLibrary = result.inLibrary;
      personalLibrary?.refresh().catch(e => toast(e.message));
      button.textContent = b.inLibrary
        ? "Убрать из моей библиотеки"
        : "+ В мою библиотеку";
      button.setAttribute("aria-pressed", String(b.inLibrary));
      button.title = b.inLibrary
        ? "Убрать из моей библиотеки"
        : "Сохранить книгу в мою библиотеку";
      toast(
        b.inLibrary
          ? "Сохранено в мою библиотеку"
          : "Убрано из моей библиотеки. Книга остаётся в каталоге",
      );
      if (state.book === b && $("bookDialog").open) renderBook();
      if (state.view === "personal") await loadCatalog();
    } finally {
      button.disabled = false;
    }
  });
  $("readBook").onclick = task(openReader);
  $("sendBook").onclick = task(async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    button.textContent = "Отправляем…";
    try {
      const result = await api("/queue", {
        book: b.id,
        device: $("targetDevice").value,
      });
      button.textContent =
        result.state === "delivered"
          ? "Уже на устройстве"
          : "В очереди на загрузку";
      toast(
        result.state === "delivered"
          ? "Книга уже на читалке"
          : "Книга отправлена в очередь читалки",
      );
      await refreshDot();
    } catch (e) {
      button.disabled = false;
      button.textContent = "Попробовать снова";
      throw e;
    }
  });
  $("bookDetails")
    .querySelectorAll("[data-shelf]")
    .forEach(
      (button) =>
        (button.onclick = task(async () => {
          const shelf =
            state.book.reading.shelf === button.dataset.shelf
              ? ""
              : button.dataset.shelf;
          await api(`/books/${b.id}/state`, { shelf });
          state.book.reading.shelf = shelf;
          $("bookDetails")
            .querySelectorAll("[data-shelf]")
            .forEach((node) =>
              node.classList.toggle("active", node.dataset.shelf === shelf),
            );
          if (["reading", "read", "want"].includes(state.view)) await loadCatalog();
        })),
    );
  $("bookDetails")
    .querySelectorAll("[data-facet]")
    .forEach(
      (button) =>
        (button.onclick = task(async () => {
          closeBook();
          await chooseFacet(button.dataset.facet, {
            value: button.dataset.value,
            label: button.dataset.label,
          }, null, false);
        })),
    );
}
function bookMetadata(b) {
  const link = (kind, value, label) =>
    `<button data-facet="${kind}" data-value="${esc(value)}" data-label="${esc(label)}">${esc(label)} →</button>`;
  return `<h3>Авторы</h3>${(b.authors || [b.author])
    .filter(Boolean)
    .map((a) => link("author", a, a))
    .join("")}
    ${b.series ? `<h3>Серия</h3>${link("series", b.series, b.series)}` : ""}
    <h3>Жанры</h3>${b.genreItems?.length ? b.genreItems.map((g) => link("genre", g.value, g.label)).join("") : '<p class="fine">Не указаны в метаданных</p>'}
    ${b.tagItems?.length ? `<details><summary>Теги · ${b.tagItems.length}</summary>${b.tagItems.map((t) => link("tag", t.value, t.label)).join("")}</details>` : ""}`;
}
function closeBook() {
  $("bookDialog").close();
  document.body.classList.remove("locked");
}
$("closeBook").onclick = closeBook;
$("bookDialog").addEventListener("close", () => {
  releaseBookDownload();
  if (!state.reader) document.body.classList.remove("locked");
});
$("bookDialog").addEventListener("click", (event) => {
  if (event.target === $("bookDialog")) {
    const box = event.target.getBoundingClientRect();
    if (
      event.clientX < box.left ||
      event.clientX > box.right ||
      event.clientY < box.top ||
      event.clientY > box.bottom
    )
      closeBook();
  }
});
async function refreshDot() {
  const queue = await api("/queue");
  $("queueDot").hidden = !queue.some((q) =>
    ["queued", "downloading"].includes(q.state),
  );
}
async function checkCatalogUpdates() {
  if (document.hidden || !state.csrf) return;
  const request = state.request, account = state.csrf;
  const result = await api("/catalog-status");
  if (request !== state.request || account !== state.csrf || document.hidden) return;
  // Preserve scroll, pagination and any open book while the index updates.
  renderCatalogStatus(result.status, result.revision);
}
function renderCatalogStatus(status, revision) {
  const changed = Number(revision) > Number(state.catalogRevision);
  const messages = {
    loading: "Обновляем каталог… Можно продолжать читать.",
    error: "Не удалось обновить каталог. Сохранённые книги доступны.",
    queued: "Обновление каталога ожидает своей очереди.",
  };
  $("catalogStatus").textContent = changed ? "В библиотеке появились изменения." : (messages[status] || "");
  $("catalogStatus").hidden = !$("catalogStatus").textContent;
  $("catalogUpdates").hidden = !changed;
}
$("catalogUpdates").onclick = task(async () => {
  const button = $("catalogUpdates");
  button.disabled = true;
  try {
    if (state.view === 'collections') await loadCollections();
    else if ($("libraryScreen").hidden) await navigate('library');
    else await loadCatalog();
  } finally {button.disabled = false;}
});
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) checkCatalogUpdates().catch(() => {});
});
async function loadQueue() {
  const request = ++state.queueRequest;
  const [devices, queue] = await Promise.all([api("/devices"), api("/queue")]);
  if (request !== state.queueRequest || state.view !== "queue") return;
  state.devices = devices;
  $("deviceList").innerHTML = devices
    .map(
      (d) =>
        `<div class="device-card">${icon("device")}<div><h3>${esc(d.name)}</h3><p>${d.last_seen ? `Была в сети ${esc(new Date(d.last_seen * 1000).toLocaleString("ru", { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" }))}` : "Подключится после установки новой прошивки"}</p></div></div>`,
    )
    .join("");
  const labels = {
    queued: "Ожидает синхронизации",
    downloading: "Загружается",
    delivered: "На устройстве",
    failed: "Нужен повтор",
    cancelled: "Отменено",
  };
  $("queueList").innerHTML = queue.length
    ? queue
        .map(
          (q) =>
            `<article class="queue-item managed-book"><button class="queue-cover" data-book="${esc(q.book)}" aria-label="Открыть карточку: ${esc(q.title)}">${cover({id:q.book, title:q.title, cover:`/reader-api/books/${encodeURIComponent(q.book)}/cover`})}</button><div><span class="delivery-label">${esc(q.action === "remove" ? (q.state === "failed" ? "Не удалось удалить" : q.state === "downloading" ? "Удаляется" : "Удалится при синхронизации") : labels[q.state] || q.state)}</span><h3><button class="queue-title" data-book="${esc(q.book)}">${esc(q.title)}</button></h3><p>${esc(devices.find((d) => d.id === q.device)?.name || "Читалка")}</p>${q.state === "failed" ? `<button class="text-button" data-retry="${q.id}">Повторить</button>` : ""}${q.action !== "remove" ? `<button class="text-button remove-book" data-remove="${q.id}">Убрать с читалки</button>` : '<p class="fine">Файл в общей и личной библиотеке останется.</p>'}</div></article>`,
        )
        .join("")
    : '<div class="empty-state">На читалку пока ничего не отправлено. Выбери «Загрузить на устройство» в карточке книги.</div>';
  bindCovers($("queueList"));
  $("queueList").querySelectorAll("[data-book]").forEach(button => {
    button.onclick = task(() => openBook(button.dataset.book));
  });
  for (const action of ["retry", "remove"])
    $("queueList")
      .querySelectorAll(`[data-${action}]`)
      .forEach(
        (button) =>
          (button.onclick = task(async () => {
            if (action === "remove" && !window.confirm("Убрать книгу с читалки при следующей синхронизации? На сайте она останется.")) return;
            button.disabled = true;
            try {
              await api(`/queue/${button.dataset[action]}/${action}`, {});
              toast(action === "remove" ? "Удалится после синхронизации читалки" : "Повтор запланирован");
              await loadQueue();
            } finally { button.disabled = false; }
          })),
      );
  $("queueDot").hidden = !queue.some((q) =>
    ["queued", "downloading"].includes(q.state),
  );
}
$("refreshQueue").onclick = task(loadQueue);

let activeUpload = null;
$("openUpload").onclick = () => { if (!activeUpload) $("uploadDialog").showModal(); };
$("closeUpload").onclick = () => $("uploadDialog").close();
$("uploadBookButton").onclick = () => { if (!activeUpload) $("uploadFile").click(); };
$("cancelBookUpload").onclick = () => {
  if (activeUpload) activeUpload.cancelled = true;
  $("uploadStatus").textContent = 'Останавливаем после текущего файла…';
};
$("uploadFile").onchange = task(async () => {
  const files = Array.from($("uploadFile").files || []);
  if (!files.length || activeUpload) return;
  $("uploadDialog").close();
  const button = $("uploadBookButton"), status = $("uploadStatus");
  const run = activeUpload = {cancelled: false, account: state.csrf, collection: state.personalCollection};
  const sameAccount = () => run.account === state.csrf;
  button.disabled = true;
  $("openUpload").disabled = true;
  button.textContent = 'Загружаю…';
  $("cancelBookUpload").hidden = false;
  $("uploadErrors").hidden = true;
  $("uploadErrors").open = false;
  $("uploadErrorsList").replaceChildren();
  try {
    const result = await window.BookPersonal.uploadBatch(files, {
      alive: () => !run.cancelled && sameAccount(),
      report: progress => {
        if (sameAccount()) status.textContent = progress.waiting
          ? `Сервер занят. Повторим через ${progress.waiting} с · ${progress.done} / ${progress.total}`
          : `Обработано ${progress.done} / ${progress.total} · добавлено ${progress.added}${progress.file ? ' · ' + progress.file : ''}`;
      },
      upload: async file => {
        const book = await api('/uploads', undefined, {method: 'POST', body: file,
            headers: {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': run.account,
              'X-Upload-Filename': encodeURIComponent(file.name), 'X-Upload-Modified': String(file.lastModified || 0)}});
          if (run.collection && sameAccount()) {
          try { await api(`/books/${book.id}/personal-collections`, {id: run.collection, selected: true}); }
          catch (error) { throw new Error(`Книга сохранена, но не добавлена в коллекцию: ${error.message}`); }
          }
          return book;
      },
    });
    if (!sameAccount()) return;
      status.textContent = `${result.stopped ? 'Очередь остановлена. ' : ''}Обработано: ${result.done} из ${result.total}. Добавлено: ${result.added}. Обновлено: ${result.replaced}. Уже есть: ${result.duplicates + result.kept}. Ошибок: ${result.errors.length}.`;
    $("uploadErrors").hidden = !result.errors.length;
    $("uploadErrorsTitle").textContent = `Не удалось завершить: ${result.errors.length}`;
    for (const error of result.errors) {
      const item = document.createElement('li'); item.textContent = `${error.name}: ${error.message}`;
      $("uploadErrorsList").append(item);
    }
    refreshPersonalLibrary();
  } finally {
    activeUpload = null;
    button.disabled = false;
    button.textContent = "Выбрать файлы";
    $("openUpload").disabled = false;
    $("cancelBookUpload").hidden = true;
    $("uploadFile").value = "";
    if (!sameAccount()) status.textContent = '';
  }
});

// One CSS column per page, one or two pages per viewport: browser line fragmentation.
// Positions stay block/character based so font and viewport changes never lose the text.
// Counts belong to this opening of a book and this layout, never to the saved bookmark.
function createBookPageIndex({ load, measure, changed, frame }) {
  const layouts = new Map();
  let active = null, generation = 0, controller = null, closed = false;
  const complete = entry => entry.counts.every(n => Number.isInteger(n) && n > 0);
  function cancel() { generation++; controller?.abort(); controller = null; }
  async function calculate(entry, geometry) {
    const ticket = ++generation;
    controller = new AbortController();
    const signal = controller.signal;
    const current = () => !closed && ticket === generation;
    entry.running = true;
    try {
      let start = 0;
      while (start < entry.counts.length) {
        if (entry.counts[start] !== null) { start++; continue; }
        const batch = await load(start, signal);
        if (!current()) return;
        if (batch.total !== entry.counts.length || !Array.isArray(batch.items) || !batch.items.length ||
            batch.next !== start + batch.items.length || batch.next > batch.total)
          throw new Error('Invalid page-count batch');
        for (const [offset, item] of batch.items.entries()) {
          if (item.chapter !== start + offset || typeof item.html !== 'string')
            throw new Error('Invalid page-count chapter');
          if (entry.counts[item.chapter] !== null) continue;
          // One chapter per frame; never replace the text being read to measure it.
          await frame();
          if (!current()) return;
          const count = measure(item.html, geometry);
          if (!Number.isInteger(count) || count < 1) throw new Error('Invalid page count');
          entry.counts[item.chapter] = count;
        }
        start = batch.next;
      }
    } catch (error) {
      if (current()) entry.error = true;
    } finally {
      if (current()) { entry.running = false; changed(); }
    }
  }
  return {
    refresh(key, total, chapter, count, geometry, retry = false) {
      if (closed || total < 1) return;
      if (active?.key !== key || active.counts.length !== total) {
        cancel();
        active = layouts.get(key);
        if (!active || active.counts.length !== total) {
          active = { key, counts: Array(total).fill(null), error: false, running: false };
          layouts.set(key, active);
        }
        active.running = false;
        active.error = false;
        while (layouts.size > 3) layouts.delete(layouts.keys().next().value);
      }
      active.counts[chapter] = count;
      if (retry) active.error = false;
      if (!active.running && !active.error && !complete(active)) calculate(active, geometry);
    },
    position(chapter, page) {
      if (!active || !complete(active)) return null;
      return {
        page: active.counts.slice(0, chapter).reduce((a, b) => a + b, 0) + page + 1,
        total: active.counts.reduce((a, b) => a + b, 0),
      };
    },
    get error() { return !!active?.error; },
    invalidate() { cancel(); active = null; layouts.clear(); },
    close() { closed = true; cancel(); active = null; layouts.clear(); },
  };
}
function createReaderBookIndex(reader) {
  const index = createBookPageIndex({
    load: (start, signal) => api(`/books/${reader.id}/read?start=${start}&limit=16`, undefined, { signal }),
    frame: () => new Promise(resolve => requestAnimationFrame(resolve)),
    changed: () => { if (state.reader === reader) updateReaderPages(); },
    measure(html, geometry) {
      const viewport = document.createElement('div'), article = document.createElement('article');
      viewport.className = 'reader-scroll reader-measure';
      viewport.setAttribute('aria-hidden', 'true');
      viewport.setAttribute('inert', '');
      viewport.style.width = `${geometry.width}px`;
      viewport.style.height = `${geometry.height}px`;
      article.innerHTML = html;
      viewport.append(article);
      $("readerScreen").append(viewport);
      try {
        const pages = createReaderPages(viewport, article, [], () => null);
        pages.layout(null);
        return pages.count;
      } finally { viewport.remove(); }
    },
  });
  return {
    position: index.position, close: index.close, invalidate: index.invalidate,
    get error() { return index.error; },
    refresh(retry = false) {
      if (state.reader !== reader || reader.loading || !reader.pages) return;
      const viewport = $("readerScroll"), style = getComputedStyle($("readerText"));
      const geometry = { width: viewport.clientWidth, height: viewport.clientHeight };
      if (geometry.width < 1 || geometry.height < 1) return;
      const key = JSON.stringify([geometry, style.fontFamily, style.fontSize, style.fontWeight,
        style.lineHeight, style.letterSpacing, style.wordSpacing]);
      index.refresh(key, reader.total, reader.chapter, reader.pages.count, geometry, retry);
    },
  };
}
function createReaderPages(viewport, article, blocks, rangeFor) {
  // `page` remains the individual page (including the right-hand speech page).
  // Only the visual translation and manual turns are grouped into spreads.
  let page = 0, count = 1, stride = 1, columns = 1;
  const spreadStart = () => Math.floor(page / columns) * columns;
  function snapshot(direction) {
    const doc = viewport.ownerDocument, win = doc?.defaultView;
    viewport.querySelectorAll?.('.reader-turn-sheet').forEach(node => node.remove());
    if (!article.animate || !doc || !win?.matchMedia('(min-width: 768px) and (min-height: 600px)').matches ||
        win?.matchMedia('(prefers-reduced-motion: reduce)').matches ||
        viewport.classList.contains('reader-measure')) return () => {};
    const sheet = doc.createElement('div'), copy = article.cloneNode(true);
    sheet.className = 'reader-turn-sheet';
    sheet.setAttribute('aria-hidden', 'true'); sheet.setAttribute('inert', '');
    copy.removeAttribute('id'); copy.querySelectorAll('[id]').forEach(node => node.removeAttribute('id'));
    sheet.style.width = `${stride}px`;
    sheet.style.left = columns === 2 && direction > 0 ? `${stride}px` : '0';
    sheet.style.transformOrigin = direction > 0 ? 'left center' : 'right center';
    copy.style.transform = `translateX(${-(spreadStart() + (columns === 2 && direction > 0 ? 1 : 0)) * stride}px)`;
    sheet.append(copy);
    // Animate only a disposable visual copy. Range geometry and speech anchors
    // on the real article must never be transformed by an in-flight animation.
    return () => {
      viewport.append(sheet);
      const animation = sheet.animate([
        {transform: 'perspective(1600px) rotateY(0deg)', opacity: 1},
        {transform: `perspective(1600px) rotateY(${direction > 0 ? -85 : 85}deg)`, opacity: 0},
      ], {duration: 260, easing: 'cubic-bezier(.2,.7,.2,1)', fill: 'forwards'});
      animation.finished.catch(() => {}).finally(() => sheet.remove());
    };
  }
  const runs = blocks.flatMap((block, index) => {
    const first = block.text.search(/\S/u), last = block.text.trimEnd().length - 1;
    return first < 0 ? [] : [{ block: index, first, last, text: block.text }];
  });
  const clamp = n => Math.max(0, Math.min(count - 1, Math.floor(n) || 0));
  const valid = a => a && Number.isInteger(a.block) && Number.isInteger(a.char) &&
    a.char >= 0 && blocks[a.block] && a.char <= blocks[a.block].text.length;
  function column(anchor) {
    const length = blocks[anchor.block]?.text.length || 0;
    const rect = rangeFor({ block: anchor.block, char: Math.min(anchor.char, Math.max(0, length - 1)) })?.getBoundingClientRect();
    return rect ? clamp((rect.left - viewport.getBoundingClientRect().left + spreadStart() * stride) / stride) : 0;
  }
  function show(next, animate = false) {
    const previous = spreadStart(), target = clamp(next);
    const finish = animate && Math.floor(target / columns) * columns !== previous
      ? snapshot(target > page ? 1 : -1) : () => {};
    page = target;
    article.style.transform = `translateX(${-spreadStart() * stride}px)`;
    viewport.scrollLeft = viewport.scrollTop = 0;
    finish();
  }
  function firstAnchor(target = page) {
    // Find the exact first visible character, including pages inside a long paragraph.
    for (const run of runs) {
      if (column({ block: run.block, char: run.last }) < target) continue;
      const ink = n => { while (n < run.last && /\s/u.test(run.text[n])) n++; return n; };
      let low = run.first, high = run.last;
      while (low < high) {
        const mid = Math.floor((low + high) / 2);
        if (column({ block: run.block, char: ink(mid) }) < target) low = mid + 1;
        else high = mid;
      }
      return { block: run.block, char: ink(low) };
    }
    const last = runs[runs.length - 1];
    return last ? { block: last.block, char: blocks[last.block].text.length } : { block: 0, char: 0 };
  }
  return {
    get page() { return page; }, get count() { return count; },
    get columns() { return columns; }, get spreadStart() { return spreadStart(); },
    get spreadEnd() { return Math.min(count - 1, spreadStart() + columns - 1); },
    get offset() { return page / Math.max(1, count - 1); },
    show, firstAnchor, snapshot,
    bounds(index = page) {
      const last = runs.at(-1);
      const anchor = firstAnchor(index), end = index + 1 < count ? firstAnchor(index + 1)
        : {block: last?.block || 0, char: last ? blocks[last.block].text.length : 0};
      return {page: index, anchor, end, empty: !last || (anchor.block === end.block && anchor.char === end.char)};
    },
    follow(anchor) { if (valid(anchor)) show(column(anchor), true); },
    layout(anchor, offset = 0) {
      if (viewport.clientWidth < 1 || viewport.clientHeight < 1) return anchor;
      viewport.querySelectorAll?.('.reader-turn-sheet').forEach(node => node.remove());
      columns = viewport.clientWidth >= 900 ? 2 : 1;
      stride = viewport.clientWidth / columns;
      viewport.classList?.toggle('reader-spread', columns === 2);
      page = 0;
      article.style.transform = "none";
      article.style.width = `${Math.max(1, stride - 44)}px`;
      // Explicit width establishes a multicol container on older iPhone WebKit.
      // column-count: 1 alone can leave a chapter as clipped vertical overflow.
      article.style.columnWidth = article.style.width;
      article.style.height = `${Math.max(1, viewport.clientHeight - 32)}px`;
      // A column plus its gap is one page wide; a spread contains two pages.
      count = Math.max(1, Math.ceil((article.scrollWidth + 44 - 1) / stride));
      show(valid(anchor) ? column(anchor) : Math.round(Math.max(0, Math.min(1, offset || 0)) * (count - 1)));
      return valid(anchor) ? { ...anchor } : firstAnchor();
    },
  };
}
function updateReaderPages() {
  const reader = state.reader;
  if (!reader) return;
  const page = reader.pages?.page || 0, count = reader.pages?.count || 1;
  const start = reader.pages?.spreadStart ?? page, end = reader.pages?.spreadEnd ?? page;
  const busy = reader.loading || reader.navigating;
  $("previousChapter").disabled = busy || (reader.chapter === 0 && start === 0);
  $("nextChapter").disabled = busy || (reader.chapter >= (reader.total || 1) - 1 && end === count - 1);
  $("chapterSelect").disabled = !!busy;
  const position = reader.bookPages?.position(reader.chapter, start);
  const label = position && end > start ? `${position.page}–${position.page + end - start}` : position?.page;
  $("readerPage").textContent = busy ? "Загрузка…" : position ? `Стр. ${label} / ${position.total}`
    : reader.bookPages?.error ? "Не удалось посчитать страницы" : "Считаем страницы…";
  $("readerScroll").setAttribute("aria-busy", String(!!busy));
}
function repaginateReader() {
  const reader = state.reader;
  if (!reader?.pages || reader.loading || reader.layoutPending) return;
  reader.layoutPending = true;
  requestAnimationFrame(() => {
    reader.layoutPending = false;
    if (state.reader !== reader || reader.loading) return;
    reader.anchor = reader.pages.layout(reader.anchor);
    reader.bookPages?.refresh();
    updateReaderPages();
    readerVoice?.reflow?.();
    savePosition().catch(() => {});
  });
}
function readerTextBlocks(article = $("readerText")) {
  const blocks = [];
  const walker = document.createTreeWalker(article, NodeFilter.SHOW_TEXT);
  let node;
  while ((node = walker.nextNode())) {
    const excluded = node.parentElement.closest("script,style,img,[aria-hidden='true']");
    // The offscreen measurement viewport is aria-hidden/inert, not the book's
    // text. Only exclusions INSIDE this article may remove speech content.
    if (excluded && article.contains(excluded)) continue;
    const element = node.parentElement.closest("p,h1,h2,h3,h4,blockquote,li") || article;
    let block = blocks[blocks.length - 1];
    if (!block || block.element !== element) {
      block = { element, text: "", nodes: [] };
      blocks.push(block);
    }
    block.nodes.push({ node, start: block.text.length });
    block.text += node.textContent;
  }
  return blocks;
}
function anchorRange(anchor, end = anchor.char + 1, blocks = state.reader?.blocks) {
  const block = blocks?.[anchor.block];
  if (!block?.nodes.length) return null;
  const locate = offset => {
    offset = Math.min(block.text.length, Math.max(0, offset));
    const item = [...block.nodes].reverse().find(n => n.start <= offset) || block.nodes[0];
    return [item.node, Math.min(item.node.length, offset - item.start)];
  };
  const range = document.createRange();
  range.setStart(...locate(anchor.char));
  range.setEnd(...locate(end));
  return range;
}
function visibleReaderAnchor() {
  return state.reader?.pages?.firstAnchor() || { block: 0, char: 0 };
}
function readerSpeechStartPosition() {
  const reader = state.reader, anchor = visibleReaderAnchor();
  // A new listening session starts at the visible page, not a stale saved cue.
  if (reader) reader.anchor = { ...anchor };
  return {book: reader?.id, chapter: reader?.chapter || 0, anchor};
}
function speechGeometry() {
  const viewport = $("readerScroll"), style = getComputedStyle($("readerText"));
  return {width: viewport.clientWidth, height: viewport.clientHeight,
    font: [style.fontFamily, style.fontSize, style.lineHeight, style.fontWeight, style.letterSpacing].join('|')};
}
function speechPage(position = null) {
  const reader = state.reader;
  if (!reader?.pages || reader.loading) throw new Error('Дождитесь загрузки страницы');
  if (position?.book === reader.id && position.chapter === reader.chapter) reader.anchor = {...position.anchor};
  // Font changes preserve the spoken text anchor in the new page geometry.
  reader.anchor = reader.pages.layout(reader.anchor);
  const geometry = speechGeometry(), key = JSON.stringify(geometry);
  reader.speechPages = {key, geometry, chapters: new Map([[reader.chapter,
    Array.from({length: reader.pages.count}, (_, i) => reader.pages.bounds(i))]])};
  updateReaderPages();
  return {book: reader.id, chapter: reader.chapter, ...reader.pages.bounds(), layout: key};
}
async function nextSpeechPage(previous) {
  const reader = state.reader, cache = reader?.speechPages;
  if (!reader || reader.id !== previous.book || cache?.key !== previous.layout) return null;
  let chapter = previous.chapter, page = previous.page + 1;
  if (page >= cache.chapters.get(chapter).length) { chapter++; page = 0; }
  if (chapter >= reader.total) return null;
  if (!cache.chapters.has(chapter)) {
    const data = await api(`/books/${reader.id}/read?chapter=${chapter}`);
    if (state.reader !== reader || reader.speechPages !== cache) return null;
    const viewport = document.createElement('div'), article = document.createElement('article');
    viewport.className = 'reader-scroll reader-measure';
    viewport.setAttribute('aria-hidden', 'true'); viewport.setAttribute('inert', '');
    viewport.style.width = `${cache.geometry.width}px`; viewport.style.height = `${cache.geometry.height}px`;
    article.innerHTML = data.html; viewport.append(article); $("readerScreen").append(viewport);
    try {
      const blocks = readerTextBlocks(article);
      const pages = createReaderPages(viewport, article, blocks, a => anchorRange(a, a.char + 1, blocks));
      pages.layout(null);
      cache.chapters.set(chapter, Array.from({length: pages.count}, (_, i) => pages.bounds(i)));
      while (cache.chapters.size > 4) cache.chapters.delete(cache.chapters.keys().next().value);
    } finally { viewport.remove(); }
  }
  return {book: reader.id, chapter, ...cache.chapters.get(chapter)[page], layout: cache.key};
}
function followReaderAnchor(anchor, force = false) {
  const reader = state.reader;
  if (!reader || reader.loading) return;
  reader.anchor = { ...anchor };
  const block = reader.blocks?.[anchor.block];
  if (!block) return;
  const previousPage = reader.pages?.page;
  reader.pages?.follow(anchor);
  updateReaderPages();
  if (readerVoice?.active) {
    if (reader.highlight !== block.element) {
      reader.highlight?.classList.remove("spoken-block");
      reader.highlight = block.element;
      reader.highlight.classList.add("spoken-block");
    }
    // Persist real progress, not a timer-based estimate of spoken text.
    if (reader.pages?.page !== previousPage || Date.now() - (reader.voiceSavedAt || 0) > 2000) {
      reader.voiceSavedAt = Date.now();
      savePosition().catch(() => {});
    }
  }
}
async function moveReaderChapter(chapter, offset = 0) {
  const reader = state.reader;
  if (!reader || reader.loading || reader.navigating || chapter < 0 || chapter >= reader.total) return;
  reader.navigating = true;
  let resume = readerVoice?.status === "playing";
  if (readerVoice?.active) {
    if (readerVoice.engine === 'server') resume = readerVoice.navigate();
    else readerVoice.pause();
  }
  updateReaderPages();
  try {
    await savePosition().catch(() => {});
    if (state.reader !== reader) return;
    await loadChapter(chapter, offset);
    if (state.reader === reader && resume) readerVoice.play();
  } finally {
    reader.navigating = false;
    if (state.reader === reader) {
      $("chapterSelect").value = reader.chapter;
      updateReaderPages();
    }
  }
}
async function turnReaderPage(delta) {
  const reader = state.reader;
  if (!reader?.pages || reader.loading || reader.navigating) return;
  const next = (reader.pages.spreadStart ?? reader.pages.page) + delta * (reader.pages.columns || 1);
  if (next < 0 || next >= reader.pages.count) {
    await moveReaderChapter(reader.chapter + delta, delta < 0 ? 1 : 0);
    return;
  }
  let resume = readerVoice?.status === "playing";
  if (readerVoice?.active) {
    if (readerVoice.engine === 'server') resume = readerVoice.navigate();
    else readerVoice.pause();
  }
  reader.pages.show(next, true);
  reader.anchor = visibleReaderAnchor();
  updateReaderPages();
  if (resume) readerVoice.play();
  clearTimeout(positionTimer);
  savePosition().catch(() => {});
}
function voiceState(status, error = "") {
  const running = status === "playing" || status === "loading";
  $("voicePreparing").hidden = !(readerVoice?.engine === 'server' && status === 'loading');
  $("voiceHint").hidden = readerVoice?.engine === 'server';
  $("voicePlay").setAttribute("aria-pressed", String(running));
  $("voicePlay").innerHTML = running
    ? '<svg viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M6 4h4v16H6zm8 0h4v16h-4z"/></svg>'
    : '<svg viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="m7 3 14 9-14 9z"/></svg>';
  $("voicePlay").title = running ? "Пауза" : status === "idle" || status === "ended" ? "Слушать" : "Продолжить озвучивание";
  $("voicePlay").setAttribute("aria-label", $("voicePlay").title);
  $("voicePlay").disabled = false;
  const messages = {
    playing: "Читаем вслух", paused: "Пауза",
    loading: "Открываем следующий раздел…", ended: "Книга закончилась", idle: "",
  };
  const errors = {
    unavailable: "В этом браузере озвучивание недоступно.",
    "load-failed": "Не удалось открыть следующий раздел. Проверьте связь и нажмите ▶.",
    "not-allowed": "Браузер не запустил голос. Нажмите ▶ ещё раз.",
    "voice-unavailable": "Голос недоступен. Выберите другой голос.",
    "language-unavailable": "Нет подходящего голоса. Добавьте его в настройках устройства.",
    stalled: "Озвучивание прервано браузером. Нажмите ▶ для продолжения.",
  };
  $("voiceStatus").textContent = status === "error"
    ? errors[error] || "Голос остановился. Выберите другой голос или нажмите ▶." : messages[status];
  if (readerVoice?.engine === 'server') {
    if (status === 'loading') $("voiceStatus").textContent = 'Подготовка озвучки. Ожидайте';
    else if (error) $("voiceStatus").textContent = error;
  }
  if (status === "idle" || status === "ended") {
    state.reader?.highlight?.classList.remove("spoken-block");
    if (state.reader) state.reader.highlight = null;
  }
  if ("mediaSession" in navigator) {
    try {
      navigator.mediaSession.playbackState = running ? "playing" : status === "idle" || status === "ended" ? "none" : "paused";
      if (status === "idle") navigator.mediaSession.metadata = null;
      else if (state.book && typeof MediaMetadata !== "undefined")
        navigator.mediaSession.metadata = new MediaMetadata({ title: state.book.title, artist: state.book.author, album: "Книжный архив" });
    } catch {} // Some browsers expose only a subset of media controls for speech.
  }
}
function configureVoice() {
  const engine = $("voiceEngine").value || 'server';
  if (readerVoice && readerVoice.engine !== engine) { readerVoice.stop(); readerVoice = null; }
  if (!readerVoice && engine === 'server') {
    readerVoice = new BookPageVoice.PageVoice(api, {
      position: () => ({book: state.reader?.id, chapter: state.reader?.chapter || 0,
        anchor: state.reader?.anchor || visibleReaderAnchor()}),
      startPosition: readerSpeechStartPosition,
      page: speechPage, next: nextSpeechPage,
      layout: () => JSON.stringify(speechGeometry()),
      follow: async (position, current = () => true) => {
        const reader = state.reader, voice = readerVoice;
        if (!current() || !reader || reader.loading || reader.id !== position.book) return false;
        if (reader.chapter !== position.chapter) {
          await loadChapter(position.chapter, 0, position.anchor, () => current() && state.reader === reader && readerVoice === voice)
            .catch(() => {});
        }
        if (current() && !reader.loading && state.reader === reader && readerVoice === voice && reader.chapter === position.chapter) {
          followReaderAnchor(position.anchor);
          return true;
        }
        return false;
      },
      state: voiceState, save: position => savePosition(position).catch(() => {}),
      finish: async position => {
        const reader = state.reader;
        if (!reader || (position && position.book !== reader.id)) return;
        await savePosition(position).catch(() => {});
        await api(`/books/${reader.id}/state`, {shelf: 'read'}).catch(() => {});
        if (state.book?.id === reader.id) state.book.reading.shelf = 'read';
      },
    }, serverSpeechInfo);
    try {
      const prefs = JSON.parse(localStorage.getItem('books-server-voice-preferences') || '{}');
      if (Number.isFinite(prefs.rate) && prefs.rate >= 0.5 && prefs.rate <= 2) readerVoice.backend.rate = prefs.rate;
      if (serverSpeechInfo.voices.some(v => v.voiceURI === prefs.voice)) readerVoice.backend.voiceURI = prefs.voice;
    } catch {}
    if ('mediaSession' in navigator) {
      for (const [action, handler] of Object.entries({play: () => readerVoice?.play(), pause: () => readerVoice?.pause(),
        stop: () => readerVoice?.stop(), previoustrack: () => turnReaderPage(-1).catch(() => {}),
        nexttrack: () => turnReaderPage(1).catch(() => {})})) {
        try { navigator.mediaSession.setActionHandler(action, handler); } catch {}
      }
    }
  }
  if (!readerVoice) {
    const backend = new BookVoice.BrowserSpeech();
    readerVoice = new BookVoice.VoiceController(backend, {
      position: () => state.reader?.anchor || visibleReaderAnchor(),
      segments: () => state.reader?.segments || [],
      follow: followReaderAnchor,
      state: voiceState,
      save: () => savePosition().catch(() => {}),
      next: async (current) => {
        const reader = state.reader;
        if (!reader || reader.chapter + 1 >= reader.total) return false;
        await savePosition();
        if (state.reader !== reader || !current()) return false;
        await loadChapter(reader.chapter + 1, 0, null, current);
        return state.reader === reader;
      },
      finish: async () => {
        const reader = state.reader;
        if (!reader) return;
        await savePosition();
        await api(`/books/${reader.id}/state`, { shelf: "read" });
        if (state.book?.id === reader.id) state.book.reading.shelf = "read";
      },
    });
    readerVoice.engine = 'browser';
    try {
      const prefs = JSON.parse(localStorage.getItem("books-voice-preferences") || "{}");
      if (Number.isFinite(prefs.rate) && prefs.rate >= 0.5 && prefs.rate <= 2) backend.rate = prefs.rate;
      backend.voiceURI = typeof prefs.voice === "string" ? prefs.voice : "";
    } catch {}
    backend.synth?.addEventListener("voiceschanged", updateVoiceChoices);
    if ("mediaSession" in navigator) {
      const actions = {
        play: () => { if (state.reader && !state.reader.loading) readerVoice.play(); },
        pause: () => readerVoice.pause(), stop: () => readerVoice.stop(),
        previoustrack: () => moveReaderChapter((state.reader?.chapter || 0) - 1).catch(() => {}),
        nexttrack: () => moveReaderChapter((state.reader?.chapter || 0) + 1).catch(() => {}),
      };
      for (const [action, handler] of Object.entries(actions)) {
        try { navigator.mediaSession.setActionHandler(action, handler); } catch {}
      }
    }
  }
  readerVoice.backend.lang = state.book?.language || "ru";
  $("voiceRate").value = readerVoice.backend.rate;
  updateVoiceRateLabel();
  updateVoiceChoices();
  // Keep the panel reachable even when one engine is unavailable, so it can be switched.
  $("listenBook").disabled = false;
}
function updateVoiceChoices() {
  if (!readerVoice) return;
  const backend = readerVoice.backend;
  const voices = backend.voices().sort((a, b) =>
    Number(b.lang.startsWith(backend.lang)) - Number(a.lang.startsWith(backend.lang)) || a.name.localeCompare(b.name));
  $("voiceSelect").innerHTML = (readerVoice.engine === 'server' ? '' : '<option value="">Автоматически</option>') + voices.map(v =>
    `<option value="${esc(v.voiceURI)}">${esc(readerVoice.engine === 'server' ? v.name.split(' · ')[0] : v.name + ' · ' + v.lang)}</option>`).join("");
  $("voiceSelect").value = voices.some(v => v.voiceURI === backend.voiceURI) ? backend.voiceURI : "";
}
function toggleVoice() {
  if (!state.reader || state.reader.loading) return;
  configureVoice();
  if (readerVoice.status === "playing" || readerVoice.status === "loading") readerVoice.pause();
  else readerVoice.play();
}
function setVoicePopover(open, restoreFocus = true) {
  const panel = $("voiceControls");
  if (open && !state.reader) return;
  const focused = panel.contains(document.activeElement);
  panel.hidden = !open;
  $("listenBook").setAttribute("aria-expanded", String(open));
  if (open) {
    configureVoice();
    $("voiceHint").hidden = readerVoice.engine === 'server';
    // Focusing a select here opens its native picker on iPhone.
    $("closeVoiceControls").focus({preventScroll: true});
  } else if (restoreFocus && focused) $("listenBook").focus({preventScroll: true});
}
$("listenBook").onclick = () => setVoicePopover($("voiceControls").hidden);
$("closeVoiceControls").onclick = () => setVoicePopover(false);
document.addEventListener("click", event => {
  if ($("voiceControls").hidden || $("voiceControls").contains(event.target) || $("listenBook").contains(event.target)) return;
  setVoicePopover(false);
  if ($("readerScroll").contains(event.target)) {
    // Dismiss with one tap; do not also turn a page underneath the popover.
    readerPointer = null;
    event.preventDefault();
    event.stopPropagation();
  }
}, true);
$("voicePlay").onclick = toggleVoice;
function updateVoiceRateLabel() {
  const label = `${Number($("voiceRate").value).toFixed(2).replace('.', ',')}×`;
  $("voiceRateValue").textContent = label;
  $("voiceRate").setAttribute('aria-valuetext', label);
}
function saveVoicePreferences() {
  try { localStorage.setItem(readerVoice.engine === 'server' ? 'books-server-voice-preferences' : "books-voice-preferences", JSON.stringify({ rate: readerVoice.backend.rate, voice: readerVoice.backend.voiceURI })); } catch {}
}
$("voiceRate").oninput = updateVoiceRateLabel;
$("voiceRate").onchange = () => {
  updateVoiceRateLabel();
  if (!readerVoice) return;
  const rate = Number($("voiceRate").value);
  if (!Number.isFinite(rate) || rate < 0.5 || rate > 2) return;
  // The server recording keeps playing at the same position; no new synthesis/seek.
  if (readerVoice.engine === 'server') readerVoice.setRate(rate);
  else {
    readerVoice.backend.rate = rate;
    if (readerVoice.status === 'playing') readerVoice.play();
  }
  saveVoicePreferences();
};
$("voiceSelect").onchange = () => {
  if (!readerVoice) return;
  if (readerVoice.engine === 'server') readerVoice.setVoice($("voiceSelect").value);
  else {
    readerVoice.backend.voiceURI = $("voiceSelect").value;
    if (readerVoice.status === "playing") readerVoice.play();
  }
  saveVoicePreferences();
};
$("voiceEngine").onchange = () => {
  readerVoice?.stop(); readerVoice = null;
  try { localStorage.setItem('books-voice-engine', $("voiceEngine").value); } catch {}
  configureVoice(); voiceState('paused');
};
window.addEventListener("resize", repaginateReader);
window.visualViewport?.addEventListener("resize", repaginateReader);
if (typeof ResizeObserver !== "undefined") new ResizeObserver(repaginateReader).observe($("readerScroll"));
document.fonts?.addEventListener("loadingdone", () => {
  state.reader?.bookPages?.invalidate();
  repaginateReader();
});
window.addEventListener("online", () => state.reader?.bookPages?.refresh(true));

function readerPreferences() {
  let preferences = { size: 20, line: 1.7, theme: "paper" };
  try {
    preferences = {
      ...preferences,
      ...JSON.parse(localStorage.getItem("books-reader-preferences") || "{}"),
    };
  } catch {}
  $("readerFontSize").value = preferences.size;
  $("readerLineHeight").value = preferences.line;
  $("readerTheme").value = preferences.theme;
  try { $("voiceEngine").value = localStorage.getItem('books-voice-engine') === 'browser' ? 'browser' : 'server'; } catch {}
  applyReaderPreferences();
}
function applyReaderPreferences() {
  const p = {
    size: Number($("readerFontSize").value),
    line: Number($("readerLineHeight").value),
    theme: $("readerTheme").value,
  };
  $("readerScreen").style.setProperty("--reader-size", `${p.size}px`);
  $("readerScreen").style.setProperty("--reader-line", p.line);
  $("readerScreen").classList.toggle("night", p.theme === "night");
  repaginateReader();
  try {
    localStorage.setItem("books-reader-preferences", JSON.stringify(p));
  } catch {}
}
function createReaderScreenAwake(nav, doc, win, now = () => Date.now()) {
  let active = false, suspended = false, lock = null, pending = null, generation = 0, retryAt = 0;
  const wanted = () => active && !suspended && !doc.hidden;
  function release() {
    const previous = lock;
    lock = null;
    if (previous) {
      try { Promise.resolve(previous.release()).catch(() => {}); } catch {}
    }
  }
  function acquire() {
    if (!wanted() || lock || pending || !nav.wakeLock?.request || now() < retryAt) return;
    const stamp = generation;
    // Serialize requests; a permission response can arrive after leaving the book.
    pending = Promise.resolve().then(() => {
      if (stamp !== generation || !wanted()) return null;
      return nav.wakeLock.request('screen');
    }).then(sentinel => {
      if (!sentinel) return;
      if (stamp !== generation || !wanted()) {
        return Promise.resolve(sentinel.release()).catch(() => {});
      }
      if (sentinel.released) { retryAt = now() + 5000; return; }
      lock = sentinel;
      sentinel.addEventListener('release', () => {
        if (lock === sentinel) lock = null;
        // Respect OS release (battery saving / manual lock), no reacquire loop.
        // A return to the visible reader or its next interaction will retry.
      });
    }).catch(() => {
      if (stamp === generation) retryAt = now() + 5000;
    }).finally(() => {
      pending = null;
      if (stamp !== generation && wanted()) acquire();
    });
  }
  function reconcile() {
    generation++; retryAt = 0;
    if (!wanted()) release();
    else acquire();
  }
  doc.addEventListener('visibilitychange', reconcile);
  win.addEventListener('pagehide', () => { suspended = true; reconcile(); });
  win.addEventListener('pageshow', () => { suspended = false; reconcile(); });
  doc.addEventListener('pointerup', acquire, {passive: true});
  doc.addEventListener('keydown', acquire);
  return {setActive(value) {
    if (active !== !!value) {active = !!value; reconcile();}
    else if (active) acquire();
  }};
}

async function openReader() {
  $("bookmarksDialog")?.close();
  readerVoice?.stop();
  setVoicePopover(false, false);
  voiceState('idle');
  state.reader?.bookPages?.close();
  const b = state.book;
  const button = $("readBook");
  button.disabled = true;
  button.textContent = "Подготовка…";
  try {
    state.reader = {
      id: b.id,
      chapter: b.reading.chapter,
      offset: b.reading.offset,
      loading: true,
      sequence: 0,
      anchor: b.reading.anchor || null,
    };
    readerPreferences();
    $("readerBookTitle").textContent = b.title;
    $("bookDialog").close();
    $("readerScreen").hidden = false;
    readerScreenAwake.setActive(true);
    document.body.classList.add("locked");
    state.reader.bookPages = createReaderBookIndex(state.reader);
    await loadChapter(state.reader.chapter, state.reader.offset, state.reader.anchor);
    configureVoice();
    api('/speech').then(info => { serverSpeechInfo = info; }).catch(() => {});
    await api(`/books/${b.id}/state`, { shelf: "reading" }).catch((e) =>
      toast(e.message),
    );
    b.reading.shelf = "reading";
  } catch (e) {
    state.reader?.bookPages?.close();
    state.reader = null;
    $("readerScreen").hidden = true;
    readerScreenAwake.setActive(false);
    if (!$("bookDialog").open) $("bookDialog").showModal();
    throw e;
  } finally {
    button.disabled = false;
    button.innerHTML = `${icon("book")} ${hasReadingPosition(b) ? "Продолжить чтение" : "Читать здесь"}`;
  }
}
async function loadChapter(chapter, offset = 0, anchor = null, current = () => true) {
  const reader = state.reader;
  if (!reader) return;
  reader.loading = true;
  const sequence = ++reader.sequence;
  // Keep the last page intact if a network request fails or is cancelled.
  updateReaderPages();
  let data;
  try {
    data = await api(`/books/${reader.id}/read?chapter=${chapter}`);
  } finally {
    if (state.reader === reader && sequence === reader.sequence) {
      reader.loading = false;
      updateReaderPages();
    }
  }
  if (state.reader !== reader || sequence !== reader.sequence || !current()) return;
  reader.loading = true;
  try {
    await new Promise((resolve, reject) => requestAnimationFrame(() => {
      try {
        if (state.reader !== reader || sequence !== reader.sequence || !current()) { resolve(); return; }
        // Apply the chapter and its page geometry together, after the final guard.
        // A cancelled audio follow must not replace text while waiting for a frame.
        const finishTurn = reader.pages?.snapshot?.(data.chapter >= reader.chapter ? 1 : -1);
        reader.chapter = data.chapter;
        reader.total = data.chapters.length;
        reader.anchor = anchor;
        $("readerText").innerHTML = data.html;
        reader.blocks = readerTextBlocks();
        reader.segments = reader.blocks.flatMap((block, index) => BookVoice.chunks(block.text, index));
        $("chapterSelect").innerHTML = data.chapters
          .map((title, index) => `<option value="${index}">${index + 1}. ${esc(title)}</option>`).join("");
        $("chapterSelect").value = data.chapter;
        reader.pages = createReaderPages($("readerScroll"), $("readerText"), reader.blocks, anchorRange);
        reader.anchor = reader.pages.layout(anchor, offset);
        finishTurn?.();
        reader.loading = false;
        reader.bookPages?.refresh();
        updateReaderPages();
        savePosition().catch(() => {});
        resolve();
      } catch (error) { reject(error); }
    }));
  } finally {
    if (state.reader === reader && sequence === reader.sequence) {
      reader.loading = false;
      updateReaderPages();
    }
  }
}
function readerPagePosition() {
  const reader = state.reader;
  if (!reader || reader.loading) return null;
  return {book: reader.id, chapter: reader.chapter,
    anchor: {...(reader.pages?.firstAnchor?.() || reader.anchor || {block: 0, char: 0})},
    offset: reader.pages?.offset ?? reader.offset ?? 0};
}
async function savePosition(spoken = null) {
  const reader = state.reader, visible = readerPagePosition();
  // Speech cues are transient. Persist only the page actually opened by either
  // navigation or audio-follow, never lookahead/preparation or a stale cue.
  if (!visible || !state.csrf || (spoken && spoken.book !== reader.id)) return;
  const {chapter, anchor, offset} = visible;
  if (state.book?.id === reader.id) {
    state.book.reading.chapter = chapter;
    state.book.reading.offset = offset;
    if (anchor) state.book.reading.anchor = anchor;
  }
  // Tiny authenticated keepalive requests may finish while iOS suspends the PWA.
  const position = { chapter, offset };
  if (anchor) position.anchor = { ...anchor };
  const saveKey = JSON.stringify(position);
  if (reader.saveKey === saveKey) return reader.saving;
  reader.saveKey = saveKey;
  // Also serialize across closing/reopening this book: the UI no longer waits
  // for close's save, so an older request must not overwrite the reopened reader.
  const queues = savePosition.queues ||= new Map(), csrf = state.csrf;
  const key = `${csrf}:${reader.id}`;
  const saving = (queues.get(key) || Promise.resolve()).catch(() => {}).then(() => {
    if (state.csrf === csrf) return api(`/books/${reader.id}/state`, position, { keepalive: true });
  });
  queues.set(key, saving);
  reader.saving = saving;
  try { await saving; }
  catch (error) { if (reader.saving === saving) reader.saveKey = null; throw error; }
  finally { if (queues.get(key) === saving) queues.delete(key); }
}
async function closeReader() {
  readerScreenAwake.setActive(false);
  $("bookmarksDialog")?.close();
  readerVoice?.stop();
  setVoicePopover(false, false);
  state.reader?.bookPages?.close();
  // Start saving while the reader still exists, but never trap navigation
  // behind a slow/offline server or a pending queue of audio bookmarks.
  const saving = savePosition().catch(() =>
    toast("Позиция не сохранена: нет связи с сервером"),
  );
  state.reader = null;
  $("readerScreen").hidden = true;
  renderBook();
  $("bookDialog").showModal();
  document.body.classList.add("locked");
  if (["personal", "reading"].includes(state.view)) loadCatalog().catch(() => {});
  await saving;
}
$("closeReader").onclick = task(closeReader);
async function jumpReaderBookmark(mark) {
  const reader = state.reader;
  if (!reader || reader.loading || reader.navigating) throw new Error('Дождитесь загрузки страницы');
  const resume = readerVoice?.engine === 'server' ? readerVoice.navigate() : readerVoice?.status === 'playing';
  if (readerVoice?.engine !== 'server') readerVoice?.pause();
  reader.navigating = true;
  try {
    await loadChapter(mark.chapter, 0, mark.anchor);
    if (state.reader === reader && resume) readerVoice.play();
  } finally {
    reader.navigating = false;
    if (state.reader === reader) updateReaderPages();
  }
}
const readerBookmarks = new BookBookmarks.Bookmarks(api, {
  reader: () => state.reader,
  position: () => {
    const position = readerPagePosition(), reader = state.reader;
    if (!position) return null;
    const page = reader.bookPages?.position(reader.chapter, reader.pages.page)?.page;
    return {...position, label: page ? `Стр. ${page}` : `Глава ${reader.chapter + 1}, стр. ${reader.pages.page + 1}`,
      excerpt: (reader.blocks?.[position.anchor.block]?.text || '').slice(position.anchor.char, position.anchor.char + 180).trim()};
  }, jump: jumpReaderBookmark, error: toast,
}, {open: $("bookmarksButton"), close: $("closeBookmarks"), dialog: $("bookmarksDialog"),
  form: $("bookmarkForm"), label: $("bookmarkLabel"), add: $("addBookmark"), list: $("bookmarkList"), status: $("bookmarkStatus")});
$("bookmarksButton").onclick = () => { setVoicePopover(false, false); readerBookmarks.open(); };
$("chapterSelect").onchange = task(async () => {
  await moveReaderChapter(Number($("chapterSelect").value));
});
for (const [id, delta] of [
  ["previousChapter", -1],
  ["nextChapter", 1],
])
  $(id).onclick = task(() => turnReaderPage(delta));
// No transparent overlays: links and native text selection remain usable.
let readerPointer = null;
$("readerScroll").onpointerdown = event => {
  readerPointer = event.isPrimary && event.button === 0
    ? { x: event.clientX, y: event.clientY, time: Date.now(), moved: false } : null;
};
$("readerScroll").onpointermove = event => {
  if (readerPointer && Math.hypot(event.clientX - readerPointer.x, event.clientY - readerPointer.y) > 10)
    readerPointer.moved = true;
};
$("readerScroll").onpointercancel = () => { readerPointer = null; };
$("readerScroll").onpointerup = task(async event => {
  const pointer = readerPointer;
  if (event.pointerType !== 'touch' || !pointer || $("readerScroll").clientWidth < 700 ||
      Date.now() - pointer.time > 700 || !window.getSelection()?.isCollapsed ||
      event.target.closest('a,button,input,select,textarea')) return;
  const dx = event.clientX - pointer.x, dy = event.clientY - pointer.y;
  if (Math.abs(dx) < 75 || Math.abs(dy) > 50) return;
  readerPointer = null; lastSwipe = Date.now();
  await turnReaderPage(dx < 0 ? 1 : -1);
});
$("readerScroll").onclick = task(async event => {
  const pointer = readerPointer;
  readerPointer = null;
  if (!pointer || pointer.moved || Date.now() - pointer.time > 500 ||
      Math.hypot(event.clientX - pointer.x, event.clientY - pointer.y) > 10 ||
      event.target.closest("a,button,input,select,textarea") ||
      window.getSelection()?.isCollapsed === false) return;
  const bounds = $("readerScroll").getBoundingClientRect();
  const x = (event.clientX - bounds.left) / bounds.width;
  if (x < 0.35) await turnReaderPage(-1);
  else if (x > 0.65) await turnReaderPage(1);
});
document.addEventListener("keydown", task(async event => {
  if (!state.reader || event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey ||
      event.target.closest("input,select,textarea,button,#voiceControls,#bookmarksDialog,[contenteditable='true']")) return;
  const delta = { ArrowLeft: -1, PageUp: -1, ArrowRight: 1, PageDown: 1 }[event.key];
  if (!delta) return;
  event.preventDefault();
  await turnReaderPage(delta);
}));
function flushReadingPosition() {
  clearTimeout(positionTimer);
  savePosition().catch(() => {});
}
document.addEventListener("visibilitychange", () => {
  if (document.hidden) flushReadingPosition();
  else if (readerVoice?.status === "playing") {
    const synth = readerVoice.backend.synth;
    if (synth && !synth.speaking && !synth.pending) readerVoice.pause();
  }
});
window.addEventListener("pagehide", flushReadingPosition);
window.addEventListener("pagehide", () => { if (readerVoice?.engine !== 'server') readerVoice?.pause(); });
$("readerSettingsButton").onclick = () => {
  const open = $("readerSettings").hidden;
  $("readerSettings").hidden = !open;
  $("readerSettingsButton").setAttribute("aria-expanded", String(open));
  repaginateReader();
};
for (const id of ["readerFontSize", "readerLineHeight", "readerTheme"])
  $(id).oninput = applyReaderPreferences;
api("/me")
  .then(enter)
  .catch(() => showLogin());
