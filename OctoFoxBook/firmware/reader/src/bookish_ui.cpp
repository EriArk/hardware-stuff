#include "bookish_ui.h"
#include <stdio.h>

namespace BookishUI {
void header(Canvas &c, unsigned activeTab, int focus) {
    c.logo(30, 28);
    c.text(Font::Heading, "AbyssBook", 87, 55, 260);
    c.text(Font::Small, "ВАША ЛИЧНАЯ БИБЛИОТЕКА", 88, 75, 300, 5);
    c.text(Font::Small, "НА УСТРОЙСТВЕ", 410, 43, 105, 5);
    c.box(30, 89, 480, 1, 0, 10, 10);
    const char *labels[] = {"Главная", "Книги", "Поиск", "Коллекции"};
    const int x[] = {30, 154, 266, 371};
    const int width[] = {113, 102, 95, 139};
    for (unsigned i = 0; i < 4; ++i) {
        bool selected = focus == static_cast<int>(i);
        if (selected) c.box(x[i] - 4, 105, width[i] + 4, 32, 4, 0, 0);
        c.text(Font::Control, labels[i], x[i], 128, width[i], selected ? 15 : 0,
               selected ? 0 : 15);
        if (i == activeTab) c.box(x[i], 144, width[i] - 10, 2, 0, 0, 0);
    }
    c.box(30, 150, 480, 1, 0, 10, 10);
}

void home(Canvas &c, const Home &v) {
    header(c, v.activeTab, v.tabFocus);
    c.text(Font::Hero, v.hero ? "Продолжим" : "Ваша личная", 30, 212, 480);
    c.text(Font::Hero, v.hero ? "историю?" : "библиотека", 30, 260, 480);
    c.text(Font::Body, v.hero ? "Тихий вечер. Хорошая книга." :
           v.empty ? "Добавьте книги через OctoFox Book." : "Выберите книгу для чтения.",
           32, 298, 478, 5);
    if (v.hero) {
        const Book &b = *v.hero;
        c.box(28, 318, 484, 266, 10, 10, 15);
        c.cover(b.id, 42, 339, 144, 222);
        c.title(b.title, 210, 367, 280);
        c.text(Font::Body, b.author, 211, 443, 278, 5);
        c.text(Font::Body, b.status, 211, 483, 279, 5);
        c.box(212, 503, 280, 2, 0, 10, 10);
        if (b.progress) c.box(212, 503, 280 * b.progress / 100, 2, 0, 0, 0);
        c.box(210, 526, 284, 43, 5, b.selected ? 0 : 5, b.selected ? 0 : 15);
        c.text(Font::Control, "Продолжить", 230, 556, 228, b.selected ? 15 : 0,
               b.selected ? 0 : 15);
        c.text(Font::Control, ">", 466, 556, 20, b.selected ? 15 : 0,
               b.selected ? 0 : 15);
    } else {
        c.box(28, 318, 484, 266, 10, 10, 15);
        c.text(Font::Title, v.empty ? "Здесь будут ваши книги" : "Начнём новую историю", 48, 394, 444);
        c.text(Font::Body, v.empty ? "Загрузите их в библиотеку на сервере," : "Откройте книгу из списка ниже", 48, 450, 444, 5);
        c.text(Font::Body, v.empty ? "затем выберите «Синхронизировать»." : "или перейдите во вкладку «Библиотека».", 48, 481, 444, 5);
    }
    c.text(Font::Heading, v.added ? "Недавно добавлены" : "Недавно открывали", 30, 653, 480);
    for (size_t i = 0; i < v.rowCount && i < 2; ++i) {
        const Book &b = v.rows[i];
        const int y = 677 + 85 * i;
        if (b.selected) c.box(28, y - 7, 484, 77, 5, 0, 15, 2);
        c.cover(b.id, 34, y, 40, 62);
        c.text(Font::Heading, b.title, 91, y + 23, 350);
        c.text(Font::Body, b.author, 92, y + 53, 340, 5);
        char progress[8];
        snprintf(progress, sizeof(progress), "%u%%", b.progress);
        c.text(Font::Body, progress, 461, y + 53, 48, 5);
        if (!b.selected) c.box(30, y + 75, 480, 1, 0, 10, 10);
    }
    c.text(Font::Small, v.notice, 30, 893, 460, 5);
    c.box(28, 904, 310, 42, 5, 0, v.syncSelected ? 0 : 15);
    c.text(Font::Body, v.syncing ? "Отменить синхронизацию" : "Синхронизировать", 42, 933, 290,
           v.syncSelected ? 15 : 0, v.syncSelected ? 0 : 15);
    c.box(354, 904, 158, 42, 5, 0, v.settingsSelected ? 0 : 15);
    c.text(Font::Control, "Настройки", 366, 933, 142,
           v.settingsSelected ? 15 : 0, v.settingsSelected ? 0 : 15);
    c.text(Font::Small, v.tabFocus >= 0 ? "UP / DOWN — вкладка · OK — открыть" :
           "OK — открыть · удержать — вкладки · 2× OK — домой", 42, 951, 468, 5);
}

void card(Canvas &c, const Card &v) {
    header(c, v.activeTab);
    c.text(Font::Hero, "О книге",30,212,480);
    c.box(28,242,484,264,10,10,15);
    c.cover(v.book.id,42,259,140,224);
    c.title(v.book.title,204,291,290);
    c.text(Font::Control,v.book.author,205,387,287,5);
    c.text(Font::Control,v.book.status,205,433,287,5);
    c.box(205,465,285,2,0,10,10);
    if(v.book.progress) c.box(205,465,285*v.book.progress/100,2,0,0,0);
    const char *labels[]={v.primary,"Аннотация",v.book.author,v.series,
        v.favorite ? "Убрать из избранного" : "В избранное","В коллекции…","< Назад"};
    for(unsigned i=0;i<7;++i) {
        const int y=525+i*52; const bool selected=v.focus==i;
        c.box(28,y,484,45,5,selected?0:15,selected?0:15);
        c.text(Font::Control,labels[i],43,y+31,450,selected?15:0,selected?0:15);
    }
    c.box(30,904,480,1,0,10,10);
    c.text(Font::Body,"OK — выбрать · удержать — вкладки",30,936,480,5);
}

void list(Canvas &c, const List &v) {
    header(c, v.activeTab, v.tabFocus);
    c.text(Font::Hero, v.title, 30, 212, 480);
    c.text(Font::Control, v.subtitle, 32, 247, 478, 5);
    if (v.back && *v.back) {
        const bool focused = v.backFocused && v.tabFocus < 0;
        c.box(28, 264, 484, 40, 5, focused ? 0 : 15, focused ? 0 : 15);
        c.text(Font::Control, v.back, 40, 292, 460, focused ? 15 : 0, focused ? 0 : 15);
    }
    if (!v.rowCount) {
        c.box(28, 327, 484, 236, 10, 10, 15);
        c.text(Font::Heading, v.emptyTitle, 48, 385, 444);
        c.text(Font::Body, v.emptyHint, 48, 436, 444, 5);
        c.text(Font::Body, v.emptyHint2, 48, 470, 444, 5);
    }
    for (size_t i = 0; i < v.rowCount && i < kListRows; ++i) {
        const auto &r = v.rows[i];
        const int y = 316 + static_cast<int>(i) * 145;
        const bool selected = r.selected && v.tabFocus < 0 && !v.backFocused;
        if (selected) {
            c.box(28, y - 5, 484, 139, 7, 0, 15, 2);
            c.box(29, y + 17, 3, 57, 0, 0, 0);
        } else c.box(30, y + 134, 480, 1, 0, 10, 10);
        const int x = r.book ? 135 : 43;
        if (r.book) c.cover(r.coverId, 42, y + 5, 77, 118);
        c.text(Font::Title, r.title, x, y + 36, 489 - x);
        c.text(Font::Control, r.subtitle, x + 1, y + 75, 487 - x, 5);
        c.text(Font::Control, r.detail, x + 1, y + 109, 470 - x, 5);
        if (r.book && r.progress) {
            c.box(x, y + 123, 354, 2, 0, 10, 10);
            c.box(x, y + 123, 354 * r.progress / 100, 2, 0, 0, 0);
        }
        if (selected) c.text(Font::Body, ">", 487, y + 109, 17);
    }
    c.box(30, 904, 480, 1, 0, 10, 10);
    if (v.total) {
        char position[48];
        snprintf(position, sizeof(position), "%lu / %lu", static_cast<unsigned long>(v.selected + 1),
                 static_cast<unsigned long>(v.total));
        c.text(Font::Control, position, 402, 932, 108, 5);
    }
    c.text(Font::Control, v.tabFocus >= 0 ? "Выберите раздел" :
           v.backFocused ? "OK — назад" : "OK — открыть", 30, 932, 355);
    c.text(Font::Body, v.tabFocus >= 0 ? "UP / DOWN — вкладка · OK — открыть" :
           "Удержать OK — вкладки · 2× OK — домой", 30, 955, 480, 5);
}
}
