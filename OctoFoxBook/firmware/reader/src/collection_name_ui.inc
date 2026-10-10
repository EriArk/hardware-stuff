// Local collection names use the same row/character interaction as Wi-Fi.
TextKeyboard collectionKeyboard;
std::atomic<bool> collectionTextEntry{false};
bool collectionNameFromCard=false;
const char *collectionNotice="";
bool displayFavorites(bool reload,const char *reason);
bool displayBookCard(const char *reason);
void paintCollectionName() {
    auto *scratch=static_cast<uint8_t*>(heap_caps_malloc(kFramebufferBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!scratch)return;
    memset(framebuffer,0xff,kFramebufferBytes); ReaderBookishCanvas c(scratch);
    using F=BookishUI::Font;
    BookishUI::header(c,3);
    c.text(F::Hero,"Новая коллекция",30,212,480);
    c.text(F::Control,"Как назовём подборку?",30,268,480,5);
    c.text(F::Title,collectionKeyboard.length()?collectionKeyboard.value():"Введите название",30,352,480);
    c.box(30,380,480,2,0,0,0);
    c.text(F::Control,collectionKeyboard.inside?"Выберите символ":"Выберите строку",30,428,480,5);
    const char *modes[]={"abc","ABC","123","#+=","абв","АБВ"};
    const char *actions[]={"Пробел","Удалить","Очистить"};
    for(unsigned row=0;row<6;++row) {
        int y=453+row*65; unsigned count=collectionKeyboard.columns(row); int width=480/count;
        c.box(28,y,484,51,4,collectionKeyboard.row==row?0:15,15);
        for(unsigned col=0;col<count;++col) {
            bool selected=collectionKeyboard.inside && collectionKeyboard.row==row && collectionKeyboard.column==col;
            int x=30+col*width; if(selected)c.box(x+1,y+3,width-2,45,3,0,0);
            char symbol[5]{}; const char *label;
            if(row<3){collectionKeyboard.symbol(row,col,symbol);label=symbol;}
            else label=row==3?modes[col]:row==4?actions[col]:"Создать коллекцию";
            c.text(F::Control,label,x+6,y+35,width-10,selected?15:0,selected?0:15);
        }
    }
    c.text(F::Body,collectionNotice,30,879,480,5);
    c.box(30,904,480,1,0,10,10);
    c.text(F::Body,"Удержать OK — назад / отменить",30,937,480,5);
    heap_caps_free(scratch);
    scheduleScreenTransitionCleanup(UiScreen::CollectionName,"collection-name");
    uiScreen=UiScreen::CollectionName;
    lastDisplayRefresh=displayRefresh.refresh(framebuffer,DisplayRefreshMode::QualityFull);
}
void openCollectionName(bool fromCard) {
    collectionNameFromCard=fromCard; collectionKeyboard.reset(); collectionKeyboard.cyrillic=true;
    collectionKeyboard.mode=4; collectionTextEntry=true; collectionNotice=""; paintCollectionName();
}
void closeCollectionName(bool created) {
    collectionTextEntry=false; collectionKeyboard.wipe(); favoritesSession.loaded=false;
    if(collectionNameFromCard) {
        if(created) bookCardSession.collectionSelection=Collections::count();
        displayBookCard("collection-name-close");
    } else {
        if(created) favoritesSession.selected=Collections::count()-1;
        displayFavorites(true,"collection-name-close");
    }
}
void inputCollectionName(const char *button,const char *gesture) {
    collectionNotice=""; const bool ok=!strcmp(button,"CENTER");
    if(ok && !strcmp(gesture,"LONG")) {
        if(collectionKeyboard.back()==TextKeyboard::Result::Back){closeCollectionName(false);return;}
    } else if(!strcmp(gesture,"SHORT")) {
        if(!strcmp(button,"UP"))collectionKeyboard.move(-1);
        else if(!strcmp(button,"DOWN"))collectionKeyboard.move(1);
        else if(ok) {
            if(collectionKeyboard.row==4 && collectionKeyboard.inside && collectionKeyboard.column==2) {
                collectionKeyboard.reset(); collectionKeyboard.mode=4;
            } else {
                auto result=collectionKeyboard.confirm();
                if(result==TextKeyboard::Result::Full)collectionNotice="Название уже достаточно длинное.";
                if(result==TextKeyboard::Result::Submit) {
                    if(Collections::create(collectionKeyboard.value())) {closeCollectionName(true);return;}
                    collectionNotice=!strcmp(Collections::error(),"collection-exists")?"Такое название уже есть.":
                        !collectionKeyboard.length()?"Введите название.":"Не сохранено. Проверьте SD и синхронизацию.";
                }
            }
        }
    } else return;
    paintCollectionName();
}
