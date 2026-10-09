#include "bookish_ui.h"
#include <stdio.h>

namespace BookishUI {
void header(Canvas &c, unsigned activeTab, int focus) {
    c.logo(30, 28);
    c.text(Font::Heading, "AbyssBook", 87, 55, 260);
    c.text(Font::Small, "ВАША ЛИЧНАЯ БИБЛИОТЕКА", 88, 75, 300, 5);
    c.text(Font::Small, "НА УСТРОЙСТВЕ", 410, 43, 105, 5);
    c.box(30, 89, 480, 1, 0, 10, 10);
    const char *labels[] = {"Главная", "Библиотека", "Поиск", "Избранное"};
    const int x[] = {30, 144, 281, 390};
    const int width[] = {94, 127, 87, 120};
    for (unsigned i = 0; i < 4; ++i) {
        bool selected = focus == static_cast<int>(i);
        if (selected) c.box(x[i] - 4, 105, width[i] + 4, 32, 4, 0, 0);
        c.text(Font::Body, labels[i], x[i], 128, width[i], selected ? 15 : 0,
               selected ? 0 : 15);
        if (i == activeTab) c.box(x[i], 144, width[i] - 10, 2, 0, 0, 0);
    }
    c.box(30, 150, 480, 1, 0, 10, 10);
}

void home(Canvas &c, const Home &v) {
    header(c, v.activeTab, v.tabFocus >= 0 ? v.tabFocus : v.sectionsSelected ? 0 : -1);
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
    c.box(28, 904, 484, 32, 5, v.syncSelected ? 0 : 15, v.syncSelected ? 0 : 15);
    c.text(Font::Body, v.syncing ? "Отменить синхронизацию" : "Синхронизировать", 42, 926, 450,
           v.syncSelected ? 15 : 0, v.syncSelected ? 0 : 15);
    c.text(Font::Small, v.tabFocus >= 0 ? "UP / DOWN — вкладка · OK — открыть" :
           "OK — открыть · удержать — вкладки · 2× OK — домой", 42, 951, 468, 5);
}
}
