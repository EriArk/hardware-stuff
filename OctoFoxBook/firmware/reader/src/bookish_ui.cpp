#include "i18n.h"
#include "bookish_ui.h"
#include <stdio.h>

namespace BookishUI {
void batteryNotice(Canvas &c, unsigned percent, bool critical) {
    if(critical) {
        c.box(0,0,540,960,0,15,15);
        c.logo(30,28);c.text(Font::Heading,"AbyssBook",87,55,360);
        c.box(30,90,480,1,0,10,10);
    } else {
        c.box(24,244,492,462,14,0,15,3);
    }
    const int y=critical?254:282;
    c.box(215,y,102,54,7,0,15,3);c.box(317,y+17,9,20,2,0,0);
    c.box(223,y+8,8,38,2,0,0);
    char charge[16]{};snprintf(charge,sizeof(charge),"%u%%",percent);
    c.text(Font::Control,charge,253,y+36,60);
    c.text(Font::Title,I18n::text(critical?"Time to recharge":"Battery running low",
        critical?"Пора зарядиться":"Мало заряда"),48,y+114,444);
    c.text(Font::Body,I18n::text("Connect a USB charger.","Подключите зарядку по USB."),48,y+163,444,4);
    if(critical) {
        c.text(Font::Body,I18n::text("Your reading progress is kept.","Позиция чтения сохранена."),48,y+204,444,4);
        c.text(Font::Body,I18n::text("The next chapter can wait.","Следующая глава подождёт."),48,y+251,444,4);
        c.box(48,771,444,1,0,10,10);
        c.text(Font::Body,I18n::text("Charge, then press Sleep to return.","После зарядки нажмите кнопку сна."),48,816,444,4);
    } else {
        c.text(Font::Body,I18n::text("Please charge before continuing.","Пожалуйста, зарядите читалку."),48,y+204,444,4);
        c.box(48,y+265,444,58,6,0,0);
        c.text(Font::Control,I18n::text("OK — continue","OK — продолжить"),66,y+303,405,15,0);
    }
}
}
#include <stdio.h>

namespace BookishUI {
void header(Canvas &c, unsigned activeTab, int focus) {
    c.logo(30, 28);
    c.text(Font::Heading, "AbyssBook", 87, 55, 260);
    c.text(Font::Small, I18n::tr("ВАША ЛИЧНАЯ БИБЛИОТЕКА"), 88, 75, 300, 5);
    c.text(Font::Small, I18n::tr("НА УСТРОЙСТВЕ"), 410, 43, 105, 5);
    c.box(30, 89, 480, 1, 0, 10, 10);
    const char *labels[] = {I18n::tr("Главная"), I18n::tr("Книги"), I18n::tr("Поиск"), I18n::tr("Коллекции")};
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
    c.text(Font::Hero, v.hero ? I18n::tr("Продолжим") : I18n::tr("Ваша личная"), 30, 212, 480);
    c.text(Font::Hero, v.hero ? I18n::tr("историю?") : I18n::tr("библиотека"), 30, 260, 480);
    c.text(Font::Body, v.hero ? I18n::tr("Тихий вечер. Хорошая книга.") :
           v.empty ? I18n::tr("Добавьте книги через OctoFox Book.") : I18n::tr("Выберите книгу для чтения."),
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
        c.text(Font::Control, I18n::tr("Продолжить"), 230, 556, 228, b.selected ? 15 : 0,
               b.selected ? 0 : 15);
        c.text(Font::Control, ">", 466, 556, 20, b.selected ? 15 : 0,
               b.selected ? 0 : 15);
    } else {
        c.box(28, 318, 484, 266, 10, 10, 15);
        c.text(Font::Title, v.empty ? I18n::tr("Здесь будут ваши книги") : I18n::tr("Начнём новую историю"), 48, 394, 444);
        c.text(Font::Body, v.empty ? I18n::tr("Загрузите их в библиотеку на сервере,") : I18n::tr("Откройте книгу из списка ниже"), 48, 450, 444, 5);
        c.text(Font::Body, v.empty ? I18n::tr("затем выберите «Синхронизировать».") : I18n::tr("или перейдите во вкладку «Библиотека»."), 48, 481, 444, 5);
    }
    c.text(Font::Heading, v.added ? I18n::tr("Недавно добавлены") : I18n::tr("Недавно открывали"), 30, 653, 480);
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
    c.text(Font::Small, v.tabFocus >= 0 ? I18n::tr("UP / DOWN — вкладка · OK — открыть") :
           I18n::tr("OK — открыть · удержать — вкладки · 2× OK — назад"), 42, 951, 468, 5);
}

void card(Canvas &c, const Card &v) {
    header(c, v.activeTab);
    c.text(Font::Hero, I18n::tr("О книге"),30,212,480);
    c.box(28,242,484,264,10,10,15);
    c.cover(v.book.id,42,259,140,224);
    c.title(v.book.title,204,291,290);
    c.text(Font::Control,v.book.author,205,387,287,5);
    c.text(Font::Control,v.book.status,205,433,287,5);
    c.box(205,465,285,2,0,10,10);
    if(v.book.progress) c.box(205,465,285*v.book.progress/100,2,0,0,0);
    const char *labels[]={v.primary,I18n::tr("Аннотация"),v.book.author,v.series,
        v.favorite ? I18n::tr("Убрать из избранного") : I18n::tr("В избранное"),I18n::tr("В коллекции…"),I18n::tr("< Назад")};
    for(unsigned i=0;i<7;++i) {
        const int y=525+i*52; const bool selected=v.focus==i;
        c.box(28,y,484,45,5,selected?0:15,selected?0:15);
        c.text(Font::Control,labels[i],43,y+31,450,selected?15:0,selected?0:15);
    }
    c.box(30,904,480,1,0,10,10);
    c.text(Font::Body,I18n::tr("OK — выбрать · удержать — вкладки"),30,936,480,5);
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
    c.text(Font::Control, v.tabFocus >= 0 ? I18n::tr("Выберите раздел") :
           v.backFocused ? I18n::tr("OK — назад") : I18n::tr("OK — открыть"), 30, 932, 355);
    c.text(Font::Body, v.navigationHint ? v.navigationHint : v.tabFocus >= 0 ? I18n::tr("UP / DOWN — вкладка · OK — открыть") :
           I18n::tr("Удержать OK — вкладки · 2× OK — назад"), 30, 955, 480, 5);
}
}
