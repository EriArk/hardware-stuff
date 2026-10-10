// Runs on the UI task; radio work is polled, never a blocking connect loop.
enum class WifiPage { Settings, Networks, Name, Password, Connecting, Result, Reading, Display, Power, Language, Library, About, Keyboard };
WifiPage wifiPage = WifiPage::Settings;
TextKeyboard wifiKeyboard;
WifiCredential wifiDraft{};
bool wifiOpenNetwork = false;
bool wifiHiddenNetwork = false;
unsigned wifiSelection = 0;
const char *wifiNotice = "";
std::atomic<bool> wifiTextEntry{false};
bool wifiNeedsRepaint=false;

bool wifiScreen() { return uiScreen == UiScreen::DeviceSettings || uiScreen == UiScreen::Wifi; }
void closeWifiSettings() {
    wifiTextEntry = false;
    WifiSetup::leave(); wifiKeyboard.wipe();
    memset(&wifiDraft, 0, sizeof(wifiDraft)); wifiNotice = "";
    networkService.invalidateConfiguration();
}
void wifiSetPage(WifiPage page) {
    wifiPage = page;
    if (page == WifiPage::Settings) wifiSelection = 0;
    wifiTextEntry = page == WifiPage::Name || page == WifiPage::Password;
}
#include "device_settings_ui.inc"
bool paintWifiSettings() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch || !framebuffer) { heap_caps_free(scratch); return false; }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    ReaderBookishCanvas c(scratch);
    using F = BookishUI::Font;
    if(deviceSettingsPage())paintDeviceSettings(c);
    else {
    c.logo(30,28); c.text(F::Heading,"AbyssBook",87,55,260);
    c.text(F::Small,I18n::tr("НАСТРОЙКИ / WI-FI"),355,49,155,5);
    c.box(30,89,480,1,0,10,10);
    const bool editing = wifiPage == WifiPage::Name || wifiPage == WifiPage::Password;
    if(wifiPage==WifiPage::Networks) {
        c.box(28,178,484,40,5,wifiSelection==0?0:15,wifiSelection==0?0:15);
        c.text(F::Control,I18n::tr("< Назад"),42,206,456,wifiSelection==0?15:0,wifiSelection==0?0:15);
    }
    c.text(F::Hero, wifiPage == WifiPage::Settings ? I18n::tr("Настройки") : editing ? I18n::tr("Подключимся") : I18n::tr("Сеть Wi-Fi"),30,162,480);
    if (editing) {
        const bool name = wifiPage == WifiPage::Name;
        c.text(F::Hero,I18n::tr("к вашей сети."),30,210,480);
        c.text(F::Control,name ? I18n::tr("Скрытая сеть") : wifiDraft.ssid,30,267,480);
        c.text(F::Small,name ? I18n::tr("НАЗВАНИЕ СЕТИ (SSID)") : wifiHiddenNetwork ? I18n::tr("ПУСТОЙ ПАРОЛЬ — ОТКРЫТАЯ СЕТЬ") : I18n::tr("ПАРОЛЬ СЕТИ"),30,317,390,5);
        char count[24]{}; snprintf(count,sizeof(count),"%u / %u",static_cast<unsigned>(wifiKeyboard.length()),static_cast<unsigned>(wifiKeyboard.limit()));
        c.text(F::Small,count,440,317,70,5);
        char shown[65]{};
        const size_t n = wifiKeyboard.length(); size_t tail = n > 24 ? n-24 : 0;
        while(tail<n && (static_cast<unsigned char>(wifiKeyboard.value()[tail])&0xc0)==0x80) ++tail;
        if (!n) snprintf(shown,sizeof(shown),"%s",name ? I18n::tr("Введите название") : I18n::tr("Введите пароль"));
        else if (wifiKeyboard.visible) snprintf(shown,sizeof(shown),"%s",wifiKeyboard.value()+tail);
        else { memset(shown,'*',n-tail); shown[n-tail]=0; }
        c.text(F::Control,shown,30,365,480,n ? 0 : 5);
        WifiCredentials::wipe(shown,sizeof(shown));
        c.box(30,382,480,2,0,0,0);
        c.text(F::Small,wifiKeyboard.inside ? I18n::tr("Выберите символ или действие") : I18n::tr("Выберите строку"),30,422,480,5);
        const char *actions[] = {I18n::tr("Пробел"),I18n::tr("Удалить"),wifiKeyboard.visible ? I18n::tr("Скрыть") : I18n::tr("Показать")};
        for (unsigned row=0;row<6;++row) {
            const int y=440+row*62;
            const unsigned count=wifiKeyboard.columns(row);
            const int width=480/count;
            c.box(28,y,484,48,4,wifiKeyboard.row==row ? 0:15,15);
            for(unsigned col=0;col<count;++col) {
                const bool selected=wifiKeyboard.inside && wifiKeyboard.row==row && wifiKeyboard.column==col;
                const int x=30+col*width;
                if(selected) c.box(x+2,y+3,width-4,42,3,0,0);
                char symbol[5]{}; const char *label;
                if(row<3) { wifiKeyboard.symbol(row,col,symbol); label=symbol; }
                else if(row==3) label=wifiKeyboard.modeLabel(col);
                else if(row==4) label=actions[col];
                else label=name ? I18n::tr("Далее") : I18n::tr("Подключить");
                c.text(row<3 ? F::Control : F::Body,label,x+4,y+33,width-8,selected?15:0,selected?0:15);
            }
        }
    } else if (wifiPage == WifiPage::Settings) {
        c.box(28,222,484,72,5,0,0);
        c.text(F::Control,"Wi-Fi",48,267,440,15,0);
        WifiCredential saved{};
        const bool configured=WifiCredentials::load(saved);
        c.text(F::Body,configured ? I18n::tr("Сохранённая сеть") : I18n::tr("Сеть ещё не выбрана"),30,348,480,5);
        if(configured) c.text(F::Control,saved.ssid,30,393,480);
        WifiCredentials::wipe(&saved,sizeof(saved));
        c.text(F::Body,I18n::tr("Wi-Fi выключен в ожидании."),30,481,480,5);
        c.text(F::Body,I18n::tr("Включается при настройке и синхронизации."),30,514,480,5);
    } else if (wifiPage == WifiPage::Networks) {
        if(WifiSetup::state()==WifiSetup::State::Scanning) {
            c.text(F::Heading,I18n::tr("Ищем сети поблизости…"),30,260,480);
            c.text(F::Body,I18n::tr("Двойной OK — отменить."),30,307,480,5);
        } else {
            const unsigned total=WifiSetup::count()+2;
            const unsigned start=((wifiSelection?wifiSelection-1:0)/6)*6;
            if(!WifiSetup::count()) c.text(F::Body,I18n::tr("Сети не найдены. Доступен Wi-Fi 2,4 ГГц."),30,833,480,5);
            for(unsigned i=start;i<total && i<start+6;++i) {
                const int y=230+(i-start)*82;
                const bool selected=i+1==wifiSelection;
                if(selected)c.box(28,y,484,72,5,0,0);
                const bool ap=i<WifiSetup::count();
                c.text(F::Control,ap ? WifiSetup::network(i).ssid : i==WifiSetup::count() ? I18n::tr("Обновить список") : I18n::tr("Скрытая сеть"),42,y+31,456,selected?15:0,selected?0:15);
                if(ap) {
                    const auto &net=WifiSetup::network(i);
                    char detail[96]{};
                    snprintf(detail,sizeof(detail),"%s · %d dBm",!net.supported?I18n::tr("Не поддерживается"):net.open?I18n::tr("Без пароля"):I18n::tr("Защищённая сеть"),net.rssi);
                    c.text(F::Small,detail,42,y+57,456,selected?15:5,selected?0:15);
                }
            }
            char page[32]{}; snprintf(page,sizeof(page),"%u / %u",wifiSelection,total);
            c.text(F::Small,page,30,861,480,5);
        }
    } else if(wifiPage==WifiPage::Connecting) {
        c.text(F::Heading,I18n::tr("Проверяем подключение…"),30,260,480);
        c.text(F::Control,wifiDraft.ssid,30,322,480);
        c.text(F::Body,I18n::tr("Это займёт несколько секунд."),30,381,480,5);
        c.text(F::Body,I18n::tr("Двойной OK — отменить."),30,414,480,5);
    } else {
        const bool ok=WifiSetup::state()==WifiSetup::State::Success;
        c.text(F::Heading,ok ? I18n::tr("Сеть сохранена") : I18n::tr("Не удалось подключиться"),30,260,480);
        c.text(F::Control,wifiDraft.ssid,30,322,480);
        c.text(F::Body,ok ? I18n::tr("Подключение к Wi-Fi проверено.") : !strcmp(WifiSetup::error(),"save-failed") ? I18n::tr("Не удалось сохранить настройки.") : I18n::tr("Проверьте пароль и доступность сети."),30,381,480,5);
        c.text(F::Body,ok ? I18n::tr("Радио снова выключено.") : I18n::tr("Прежняя сеть осталась сохранённой."),30,414,480,5);
        c.text(F::Body,I18n::tr("Привязка к библиотеке — через Companion."),30,467,480,5);
        c.box(28,536,484,56,5,0,0);
        c.text(F::Control,ok ? I18n::tr("Готово") : wifiOpenNetwork ? I18n::tr("Выбрать другую сеть") : I18n::tr("Изменить пароль"),48,573,444,15,0);
    }
    c.text(F::Small,wifiNotice,30,878,480,5);
    c.box(30,903,480,1,0,10,10);
    c.text(F::Small,editing ? (wifiKeyboard.inside ? I18n::tr("UP / DOWN — символ · OK — ввести") : I18n::tr("UP / DOWN — строка · OK — выбрать")) : I18n::tr("UP / DOWN — выбор · OK — открыть"),30,928,480,5);
    c.text(F::Small,I18n::tr("2× OK — назад · Удержать — вкладки"),30,951,480,5);
    }
    heap_caps_free(scratch);
    const UiScreen target=deviceSettingsPage() ? UiScreen::DeviceSettings : UiScreen::Wifi;
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
    wifiKeyboard.configure(activeReaderSettings.keyboardLayouts);
    wifiSetPage(WifiPage::Settings); wifiSelection=0; wifiNotice=""; paintWifiSettings();
}
void wifiBeginScan() {
    wifiKeyboard.configure(activeReaderSettings.keyboardLayouts);
    wifiKeyboard.wipe(); WifiCredentials::wipe(&wifiDraft,sizeof(wifiDraft));
    wifiSelection=0; wifiNotice=""; wifiSetPage(WifiPage::Networks);
    WifiSetup::scan(); paintWifiSettings();
}
void wifiConnectDraft() {
    if(wifiHiddenNetwork) wifiOpenNetwork=!wifiDraft.password[0];
    if(!WifiCredentials::validPassword(wifiDraft.password,wifiOpenNetwork)) {
        wifiNotice=I18n::text("Password: 8–63 UTF-8 bytes or 64 hex digits.","Пароль: 8–63 байта UTF-8 или 64 hex-цифры."); return;
    }
    if(WifiSetup::connect(wifiDraft,wifiOpenNetwork)) { wifiKeyboard.visible=false; wifiSetPage(WifiPage::Connecting); }
    else wifiNotice=I18n::tr("Не удалось начать подключение.");
}
void inputWifiSettings(const char *button,const char *gesture) {
    wifiNotice="";
    if(inputDeviceSettings(button,gesture))return;
    const bool ok=!strcmp(button,"CENTER"), back=ok && !strcmp(gesture,"DOUBLE");
    const bool shortPress=!strcmp(gesture,"SHORT");
    if(!ok && !shortPress) return;
    if(wifiPage==WifiPage::Networks && ok && shortPress && wifiSelection==0) {
        WifiSetup::cancel();wifiSetPage(WifiPage::Settings);wifiSelection=5;paintWifiSettings();return;
    }
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
                const unsigned count=WifiSetup::count()+3;
                if(!strcmp(button,"UP")) wifiSelection=(wifiSelection+count-1)%count;
                if(!strcmp(button,"DOWN")) wifiSelection=(wifiSelection+1)%count;
            } else if(wifiSelection==WifiSetup::count()+1) { wifiBeginScan(); return; }
            else if(wifiSelection==WifiSetup::count()+2) { wifiKeyboard.reset("",32); wifiKeyboard.visible=true; wifiSetPage(WifiPage::Name); }
            else {
                const auto &ap=WifiSetup::network(wifiSelection-1);
                if(!ap.supported) wifiNotice=I18n::tr("Для WEP и корпоративных сетей нужен другой способ входа.");
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
                if(result==TextKeyboard::Result::Full) wifiNotice=I18n::tr("Достигнута максимальная длина.");
                if(result==TextKeyboard::Result::Submit) {
                    if(wifiPage==WifiPage::Name) {
                        if(!wifiKeyboard.length()) wifiNotice=I18n::tr("Введите название сети.");
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
    const bool changed=WifiSetup::poll();
    if(!wifiScreen())return;
    if(!changed) {
        if(wifiNeedsRepaint && !batteryWarningVisible) {wifiNeedsRepaint=false;paintWifiSettings();}
        return;
    }
    if(wifiPage==WifiPage::Connecting) {
        wifiSetPage(WifiPage::Result);
        if(WifiSetup::state()==WifiSetup::State::Success) {
            wifiKeyboard.wipe(); WifiCredentials::wipe(wifiDraft.password,sizeof(wifiDraft.password)); networkService.invalidateConfiguration();
        }
    } else if(WifiSetup::state()==WifiSetup::State::Failed) wifiNotice=I18n::tr("Поиск не завершён. Попробуйте обновить список.");
    if(batteryWarningVisible)wifiNeedsRepaint=true;
    else paintWifiSettings();
}
