#include "bookish_ui.h"
#include "section_back_focus.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
using namespace BookishUI;

struct Capture : Canvas {
    int logos=0, covers=0;
    void box(int x,int y,int w,int h,int r,uint8_t b,uint8_t f,int t=1) override {
        assert(x>=0 && y>=0 && w>0 && h>0 && x+w<=540 && y+h<=960);
        std::cout<<"[\"box\","<<x<<","<<y<<","<<w<<","<<h<<","<<r<<","<<int(b)<<","<<int(f)<<","<<t<<"]\n";
    }
    void text(Font font,const char *value,int x,int y,int width,uint8_t ink=0,uint8_t paper=15) override {
        assert(value && x>=0 && y>0 && y<=960 && width>0 && x+width<=540);
        std::cout<<"[\"text\","<<int(font)<<","<<std::quoted(value)<<","<<x<<","<<y<<","<<width<<","<<int(ink)<<","<<int(paper)<<"]\n";
    }
    void title(const char *value,int x,int y,int width) override { text(Font::Title,value,x,y,width); }
    void logo(int x,int y) override {++logos;std::cout<<"[\"logo\","<<x<<","<<y<<"]\n";}
    void cover(const char *,int x,int y,int w,int h) override {
        ++covers;std::cout<<"[\"cover\","<<x<<","<<y<<","<<w<<","<<h<<"]\n";
    }
};
int main() {
    bool back=false;
    using A=SectionBackFocus::Action;
    assert(SectionBackFocus::input(back,0,3,-1,false)==A::Redraw && back);
    assert(SectionBackFocus::input(back,0,3,1,false)==A::Redraw && !back);
    assert(SectionBackFocus::input(back,1,3,-1,false)==A::None);
    assert(SectionBackFocus::input(back,0,0,0,true)==A::Back && !back);
    back=true;
    assert(SectionBackFocus::input(back,0,0,1,false)==A::Redraw && back);
    assert(SectionBackFocus::input(back,0,3,0,true)==A::Back && !back);
    List v{};v.title="Ваша библиотека";v.subtitle="На устройстве: 24 · выберите подборку";
    v.total=8;v.rowCount=5;
    const char *titles[]={"Все книги","Недавно добавлены","Читаю","Непрочитанные","Прочитанные"};
    const char *hints[]={"Все загруженные книги","Последние пополнения","Истории, которые вы начали","Откройте что-нибудь новое","Прочитанные истории"};
    for(size_t i=0;i<5;++i) v.rows[i]={titles[i],hints[i],"Книг: 12","",false,i==0,0};
    Capture c;
    auto scene=[&](const char *name){std::cout<<"SCENE "<<name<<"\n";const int before=c.logos;list(c,v);assert(c.logos==before+1);};
    scene("library");
    v.title="Все книги";v.subtitle="Книг: 24";v.back="< К разделам библиотеки";
    for(size_t i=0;i<5;++i)v.rows[i]={i==0?"Очень длинное название книги, которое не должно налезть на соседнюю строку":"Сад за горизонтом","Александр Лис","Глава 8 · 34%","sample",true,i==0,34};
    scene("books");assert(c.covers==5);
    v.backFocused=true;scene("back");v.backFocused=false;
    v.tabFocus=2;scene("tabs");v.tabFocus=-1;
    v.activeTab=2;v.title="Найдём книгу";v.subtitle="Поиск среди книг на устройстве.";v.back="";
    const char *letters[]={"А–Д","Е–К","Л–П","Р–У","Ф–Я"};
    for(size_t i=0;i<5;++i)v.rows[i]={letters[i],"Выбрать первую букву","","",false,i==0,0};
    scene("search");
    v.activeTab=3;v.title="Избранное";v.subtitle="Ваши книги — в ваших подборках.";v.rowCount=3;v.total=3;
    v.rows[0]={"Хочу прочитать","Истории, с которыми хочется познакомиться","Книг: 4","",false,true,0};
    v.rows[1]={"Любимые книги","То, к чему хочется возвращаться","Книг: 2","",false,false,0};
    v.rows[2]={"На потом","Сохранено для другого настроения","Книг: 0","",false,false,0};
    scene("favorites");
    v.title="На потом";v.subtitle="Сохранённые вами истории.";v.rowCount=0;v.total=0;
    v.back="< Ко всем подборкам";v.backFocused=true;
    v.emptyTitle="В подборке пока пусто";v.emptyHint="Откройте карточку книги и добавьте";v.emptyHint2="её в одну из подборок избранного.";
    scene("empty");
}
