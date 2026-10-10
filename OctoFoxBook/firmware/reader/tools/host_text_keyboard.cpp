#include "text_keyboard.h"
#include "reader_input_policy.h"
#include <cassert>
#include <set>
#include <string>
#include <iostream>
int main() {
    TextKeyboard k;
    std::set<char> reachable{' '};
    for(unsigned mode=0;mode<4;++mode) {
        k.mode=mode;
        for(unsigned row=0;row<3;++row)
            for(unsigned col=0;col<k.columns(row);++col) {
                k.reset(); k.mode=mode; k.row=row; k.column=col; k.inside=true;
                const char wanted=k.characters(row)[col];
                assert(k.confirm()==TextKeyboard::Result::Changed);
                assert(k.length()==1 && k.value()[0]==wanted);
                assert(k.inside && k.column==col); reachable.insert(wanted);
            }
    }
    for(char c=32;c<127;++c) assert(reachable.count(c));
    k.reset(); k.move(-1); assert(k.row==5);
    assert(k.confirm()==TextKeyboard::Result::Submit);
    k.move(1); assert(k.row==0); k.confirm(); k.move(-1);
    assert(k.column==9); k.confirm(); assert(std::string(k.value())=="p");
    assert(k.back()==TextKeyboard::Result::Changed && !k.inside);
    assert(k.back()==TextKeyboard::Result::Back);
    k.reset("a",2); k.confirm(); k.confirm();
    assert(k.length()==2 && k.confirm()==TextKeyboard::Result::Full);
    k.row=4;k.column=1;k.confirm();assert(k.length()==1);
    k.column=2;k.confirm();assert(k.visible);
    k.wipe();assert(k.length()==0 && !k.visible && !k.inside);
    k.reset("abc",32); k.row=3;k.inside=true;k.column=1;k.confirm();
    assert(k.mode==1 && k.row==0 && !k.inside && std::string(k.value())=="abc");
    using E=OkGesture::Event;
    OkGesture g;
    assert(g.update(true,0,false)==E::None);
    assert(g.update(false,40,false)==E::Short);
    assert(g.update(true,80,false)==E::None);
    assert(g.update(false,120,false)==E::Short); // Rapid repeated characters, no Home.
    assert(g.update(true,200,false)==E::None);
    assert(g.update(true,1000,false)==E::Long);
    assert(g.update(false,1050,true)==E::None); // Returning from editor while held.
    assert(g.update(true,1200,true)==E::None);
    assert(g.update(false,1250,true)==E::None);
    assert(g.update(true,1300,true)==E::None);
    assert(g.update(false,1350,true)==E::Double); // Home restored outside editor.
    std::cout << "KEYBOARD_OK\n";
    k.reset("",3);k.configure(3);k.layout=TextKeyboard::Russian;k.mode=0;k.inside=true;
    assert(k.columns(0)==12);k.confirm();assert(std::string(k.value())=="й");
    assert(k.confirm()==TextKeyboard::Result::Full && k.length()==2);
    k.row=4;k.column=1;k.confirm();assert(k.length()==0);
    k.reset();k.mode=1;k.inside=true;k.confirm();assert(std::string(k.value())=="Й");
    // Every enabled layout and every codepoint survives insert/delete. A layout
    // switch keeps the draft and cycles only through the user's enabled set.
    k.configure(TextKeyboard::AllLayouts);
    for(unsigned layout=0;layout<TextKeyboard::LayoutCount;++layout) {
        k.layout=layout;
        for(unsigned mode=0;mode<2;++mode) for(unsigned row=0;row<3;++row) {
            k.mode=mode;
            const unsigned count=k.columns(row);
            for(unsigned col=0;col<count;++col) {
                k.reset();k.mode=mode;k.row=row;k.column=col;k.inside=true;
                char expected[5];k.symbol(row,col,expected);
                assert(*expected && k.confirm()==TextKeyboard::Result::Changed);
                assert(std::string(k.value())==expected);
                k.row=4;k.column=1;k.confirm();assert(k.length()==0);
            }
        }
    }
    k.configure((1U<<TextKeyboard::English)|(1U<<TextKeyboard::Spanish));
    k.layout=TextKeyboard::English;k.reset("draft");k.row=3;k.column=4;k.inside=true;
    k.confirm();assert(k.layout==TextKeyboard::Spanish && std::string(k.value())=="draft");
    k.confirm();assert(k.layout==TextKeyboard::English);
    k.configure(0);assert(k.enabledLayouts==1);
    k.reset("aй",2);assert(std::string(k.value())=="a");
}
