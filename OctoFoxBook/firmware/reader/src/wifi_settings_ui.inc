// Runs on the UI task; radio work is polled, never a blocking connect loop.
enum class WifiPage { Settings, Networks, Name, Password, Connecting, Result };
WifiPage wifiPage = WifiPage::Settings;
TextKeyboard wifiKeyboard;
WifiCredential wifiDraft{};
bool wifiOpenNetwork = false;
bool wifiHiddenNetwork = false;
unsigned wifiSelection = 0;
const char *wifiNotice = "";
std::atomic<bool> wifiTextEntry{false};

bool wifiScreen() { return uiScreen == UiScreen::DeviceSettings || uiScreen == UiScreen::Wifi; }
void closeWifiSettings() {
    wifiTextEntry = false;
    WifiSetup::leave(); wifiKeyboard.wipe();
    memset(&wifiDraft, 0, sizeof(wifiDraft)); wifiNotice = "";
    networkService.invalidateConfiguration();
}
void wifiSetPage(WifiPage page) {
    wifiPage = page;
    wifiTextEntry = page == WifiPage::Name || page == WifiPage::Password;
}
bool paintWifiSettings() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch || !framebuffer) { heap_caps_free(scratch); return false; }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    ReaderBookishCanvas c(scratch);
    using F = BookishUI::Font;
    c.logo(30,28); c.text(F::Heading,"AbyssBook",87,55,260);
    c.text(F::Small,"НАСТРОЙКИ / WI-FI",355,49,155,5);
    c.box(30,89,480,1,0,10,10);
    const bool editing = wifiPage == WifiPage::Name || wifiPage == WifiPage::Password;
    c.text(F::Hero, wifiPage == WifiPage::Settings ? "Настройки" : editing ? "Подключимся" : "Сеть Wi-Fi",30,162,480);
    if (editing) {
        const bool name = wifiPage == WifiPage::Name;
        c.text(F::Hero,"к вашей сети.",30,210,480);
        c.text(F::Control,name ? "Скрытая сеть" : wifiDraft.ssid,30,267,480);
        c.text(F::Small,name ? "НАЗВАНИЕ СЕТИ (SSID)" : wifiHiddenNetwork ? "ПУСТОЙ ПАРОЛЬ — ОТКРЫТАЯ СЕТЬ" : "ПАРОЛЬ СЕТИ",30,317,390,5);
        char count[24]{}; snprintf(count,sizeof(count),"%u / %u",static_cast<unsigned>(wifiKeyboard.length()),static_cast<unsigned>(wifiKeyboard.limit()));
        c.text(F::Small,count,440,317,70,5);
        char shown[65]{};
        const size_t n = wifiKeyboard.length(), tail = n > 24 ? n-24 : 0;
        if (!n) snprintf(shown,sizeof(shown),"%s",name ? "Введите название" : "Введите пароль");
        else if (wifiKeyboard.visible) snprintf(shown,sizeof(shown),"%s",wifiKeyboard.value()+tail);
        else { memset(shown,'*',n-tail); shown[n-tail]=0; }
        c.text(F::Control,shown,30,365,480,n ? 0 : 5);
        WifiCredentials::wipe(shown,sizeof(shown));
        c.box(30,382,480,2,0,0,0);
        c.text(F::Small,wifiKeyboard.inside ? "Выберите символ или действие" : "Выберите строку",30,422,480,5);
        const char *modes[] = {"abc","ABC","123","#+="};
        const char *actions[] = {"Пробел","Удалить",wifiKeyboard.visible ? "Скрыть" : "Показать"};
        for (unsigned row=0;row<6;++row) {
            const int y=440+row*62;
            const unsigned count=wifiKeyboard.columns(row);
            const int width=480/count;
            c.box(28,y,484,48,4,wifiKeyboard.row==row ? 0:15,15);
            for(unsigned col=0;col<count;++col) {
                const bool selected=wifiKeyboard.inside && wifiKeyboard.row==row && wifiKeyboard.column==col;
                const int x=30+col*width;
                if(selected) c.box(x+2,y+3,width-4,42,3,0,0);
                char symbol[2]{}; const char *label;
                if(row<3) { symbol[0]=wifiKeyboard.characters(row)[col]; label=symbol; }
                else if(row==3) label=modes[col];
                else if(row==4) label=actions[col];
                else label=name ? "Далее" : "Подключить";
                c.text(row<3 ? F::Control : F::Body,label,x+10,y+33,width-18,selected?15:0,selected?0:15);
            }
        }
    } else if (wifiPage == WifiPage::Settings) {
        c.box(28,222,484,72,5,0,0);
        c.text(F::Control,"Wi-Fi",48,267,440,15,0);
        WifiCredential saved{};
        const bool configured=WifiCredentials::load(saved);
        c.text(F::Body,configured ? "Сохранённая сеть" : "Сеть ещё не выбрана",30,348,480,5);
        if(configured) c.text(F::Control,saved.ssid,30,393,480);
        WifiCredentials::wipe(&saved,sizeof(saved));
        c.text(F::Body,"Wi-Fi выключен в ожидании.",30,481,480,5);
        c.text(F::Body,"Включается при настройке и синхронизации.",30,514,480,5);
    } else if (wifiPage == WifiPage::Networks) {
        if(WifiSetup::state()==WifiSetup::State::Scanning) {
            c.text(F::Heading,"Ищем сети поблизости…",30,260,480);
            c.text(F::Body,"Удержите OK, чтобы отменить.",30,307,480,5);
        } else {
            const unsigned total=WifiSetup::count()+2;
            const unsigned start=(wifiSelection/7)*7;
            if(!WifiSetup::count()) c.text(F::Body,"Сети не найдены. Доступен Wi-Fi 2,4 ГГц.",30,203,480,5);
            for(unsigned i=start;i<total && i<start+7;++i) {
                const int y=230+(i-start)*82;
                const bool selected=i==wifiSelection;
                if(selected)c.box(28,y,484,72,5,0,0);
                const bool ap=i<WifiSetup::count();
                c.text(F::Control,ap ? WifiSetup::network(i).ssid : i==WifiSetup::count() ? "Обновить список" : "Скрытая сеть",42,y+31,456,selected?15:0,selected?0:15);
                if(ap) {
                    const auto &net=WifiSetup::network(i);
                    char detail[96]{};
                    snprintf(detail,sizeof(detail),"%s · %d dBm",!net.supported?"Не поддерживается":net.open?"Без пароля":"Защищённая сеть",net.rssi);
                    c.text(F::Small,detail,42,y+57,456,selected?15:5,selected?0:15);
                }
            }
            char page[32]{}; snprintf(page,sizeof(page),"%u / %u",wifiSelection+1,total);
            c.text(F::Small,page,30,842,480,5);
        }
    } else if(wifiPage==WifiPage::Connecting) {
        c.text(F::Heading,"Проверяем подключение…",30,260,480);
        c.text(F::Control,wifiDraft.ssid,30,322,480);
        c.text(F::Body,"Это займёт несколько секунд.",30,381,480,5);
        c.text(F::Body,"Удержите OK, чтобы отменить.",30,414,480,5);
    } else {
        const bool ok=WifiSetup::state()==WifiSetup::State::Success;
        c.text(F::Heading,ok ? "Сеть сохранена" : "Не удалось подключиться",30,260,480);
        c.text(F::Control,wifiDraft.ssid,30,322,480);
        c.text(F::Body,ok ? "Подключение к Wi-Fi проверено." : !strcmp(WifiSetup::error(),"save-failed") ? "Не удалось сохранить настройки." : "Проверьте пароль и доступность сети.",30,381,480,5);
        c.text(F::Body,ok ? "Радио снова выключено." : "Прежняя сеть осталась сохранённой.",30,414,480,5);
        c.text(F::Body,"Привязка к библиотеке — через Companion.",30,467,480,5);
        c.box(28,536,484,56,5,0,0);
        c.text(F::Control,ok ? "Готово" : wifiOpenNetwork ? "Выбрать другую сеть" : "Изменить пароль",48,573,444,15,0);
    }
    c.text(F::Small,wifiNotice,30,878,480,5);
    c.box(30,903,480,1,0,10,10);
    c.text(F::Small,editing ? (wifiKeyboard.inside ? "UP / DOWN — символ · OK — ввести" : "UP / DOWN — строка · OK — выбрать") : "UP / DOWN — выбор · OK — открыть",30,928,480,5);
    c.text(F::Small,"Удержать OK — назад",30,951,480,5);
    heap_caps_free(scratch);
    const UiScreen target=wifiPage==WifiPage::Settings ? UiScreen::DeviceSettings : UiScreen::Wifi;
    const bool transition=uiScreen!=target;
    scheduleScreenTransitionCleanup(target,"wifi-settings");
    uiScreen=target;
    lastDisplayRefresh=displayRefresh.refresh(framebuffer,transition ? DisplayRefreshMode::QualityFull : DisplayRefreshMode::QualityFull);
    hasDisplayRefresh=lastDisplayRefresh.ok;
    // No SSID or password in diagnostics.
    Serial.printf("WIFI UI page=%u radio=%s\n",static_cast<unsigned>(wifiPage),WiFi.getMode()==WIFI_OFF?"off":"on");
    return lastDisplayRefresh.ok;
}
void openWifiSettings() {
    if(provisioningActive || bookUpload.active() || BookPreparation::busy() || !WifiSetup::enter()) return;
    wifiSetPage(WifiPage::Settings); wifiNotice=""; paintWifiSettings();
}
void wifiBeginScan() {
    wifiKeyboard.wipe(); WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft));
    wifiSelection=0; wifiNotice=""; wifiSetPage(WifiPage::Networks);
    WifiSetup::scan(); paintWifiSettings();
}
void wifiConnectDraft() {
    if(wifiHiddenNetwork) wifiOpenNetwork=!wifiDraft.password[0];
    if(!WifiCredentials::validPassword(wifiDraft.password,wifiOpenNetwork)) {
        wifiNotice="Пароль: 8–63 символа или 64 шестнадцатеричных знака."; return;
    }
    if(WifiSetup::connect(wifiDraft,wifiOpenNetwork)) { wifiKeyboard.visible=false; wifiSetPage(WifiPage::Connecting); }
    else wifiNotice="Не удалось начать подключение.";
}
void inputWifiSettings(const char *button,const char *gesture) {
    wifiNotice="";
    const bool ok=!strcmp(button,"CENTER"), back=ok && !strcmp(gesture,"LONG");
    const bool shortPress=!strcmp(gesture,"SHORT");
    if(!ok && !shortPress) return;
    if(back) {
        if(wifiPage==WifiPage::Settings) { closeWifiSettings(); displayHome(false,"settings-back"); return; }
        if(wifiPage==WifiPage::Name || wifiPage==WifiPage::Password) {
            if(wifiKeyboard.back()==TextKeyboard::Result::Back) {
                wifiKeyboard.wipe(); WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft)); wifiSetPage(WifiPage::Networks);
            }
        } else if(wifiPage==WifiPage::Connecting) {
            WifiSetup::cancel(); wifiSetPage(wifiOpenNetwork ? WifiPage::Networks : WifiPage::Password);
        }
        else {
            WifiSetup::cancel(); wifiKeyboard.wipe(); WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft));
            wifiSetPage(wifiPage==WifiPage::Result ? WifiPage::Networks : WifiPage::Settings);
        }
    } else if(shortPress) {
        if(wifiPage==WifiPage::Settings) { if(ok) { wifiBeginScan(); return; } }
        else if(wifiPage==WifiPage::Networks && WifiSetup::state()!=WifiSetup::State::Scanning) {
            if(!ok) {
                const unsigned count=WifiSetup::count()+2;
                if(!strcmp(button,"UP")) wifiSelection=(wifiSelection+count-1)%count;
                if(!strcmp(button,"DOWN")) wifiSelection=(wifiSelection+1)%count;
            } else if(wifiSelection==WifiSetup::count()) { wifiBeginScan(); return; }
            else if(wifiSelection==WifiSetup::count()+1) { wifiKeyboard.reset("",32); wifiKeyboard.visible=true; wifiSetPage(WifiPage::Name); }
            else {
                const auto &ap=WifiSetup::network(wifiSelection);
                if(!ap.supported) wifiNotice="Для WEP и корпоративных сетей нужен другой способ входа.";
                else {
                    WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft)); snprintf(wifiDraft.ssid,sizeof(wifiDraft.ssid),"%s",ap.ssid); wifiOpenNetwork=ap.open; wifiHiddenNetwork=false;
                    WifiCredential saved{};
                    if(!ap.open && WifiCredentials::load(saved) && !strcmp(saved.ssid,ap.ssid)) snprintf(wifiDraft.password,sizeof(wifiDraft.password),"%s",saved.password);
                    WifiCredentials::wipe(&saved,sizeof(saved)); wifiKeyboard.reset(wifiDraft.password,64);
                    wifiSetPage(WifiPage::Password);
                    if(ap.open) wifiConnectDraft();
                }
            }
        } else if(wifiPage==WifiPage::Name || wifiPage==WifiPage::Password) {
            if(!ok) {
                if(!strcmp(button,"UP")) wifiKeyboard.move(-1);
                if(!strcmp(button,"DOWN")) wifiKeyboard.move(1);
            } else {
                const auto result=wifiKeyboard.confirm();
                if(result==TextKeyboard::Result::Full) wifiNotice="Достигнута максимальная длина.";
                if(result==TextKeyboard::Result::Submit) {
                    if(wifiPage==WifiPage::Name) {
                        if(!wifiKeyboard.length()) wifiNotice="Введите название сети.";
                        else {
                            WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft)); snprintf(wifiDraft.ssid,sizeof(wifiDraft.ssid),"%s",wifiKeyboard.value());
                            wifiOpenNetwork=false; wifiHiddenNetwork=true; wifiKeyboard.reset("",64); wifiSetPage(WifiPage::Password);
                        }
                    } else { snprintf(wifiDraft.password,sizeof(wifiDraft.password),"%s",wifiKeyboard.value()); wifiConnectDraft(); }
                }
            }
        } else if(wifiPage==WifiPage::Result && ok) {
            if(WifiSetup::state()==WifiSetup::State::Success) { wifiSetPage(WifiPage::Settings); WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft)); }
            else if(wifiOpenNetwork) wifiSetPage(WifiPage::Networks);
            else wifiSetPage(WifiPage::Password);
        }
    }
    noteUiNavigationClick(); paintWifiSettings();
}
void pollWifiSettings() {
    if(!WifiSetup::poll() || !wifiScreen()) return;
    if(wifiPage==WifiPage::Connecting) {
        wifiSetPage(WifiPage::Result);
        if(WifiSetup::state()==WifiSetup::State::Success) {
            wifiKeyboard.wipe(); WifiCredentials::wipe(wifiDraft.password,sizeof(wifiDraft.password)); networkService.invalidateConfiguration();
        }
    } else if(WifiSetup::state()==WifiSetup::State::Failed) wifiNotice="Поиск не завершён. Попробуйте обновить список.";
    paintWifiSettings();
}
