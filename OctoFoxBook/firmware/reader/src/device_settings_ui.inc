// Device-wide settings use the same header, rows and visible Back as Library.
bool paintWifiSettings();
void wifiBeginScan();
bool deviceSettingsPage() {
    return wifiPage == WifiPage::Settings || wifiPage >= WifiPage::Reading;
}
const char *deviceText(const char *en, const char *ru) { return I18n::text(en, ru); }
void deviceSettingsBack() {
    if (wifiPage != WifiPage::Settings) {
        wifiSetPage(WifiPage::Settings); wifiSelection = 0; return;
    }
    closeWifiSettings();
    localLibrarySession.owner = TopLevelTab::OnDevice;
    localLibrarySession.phase = LocalLibraryPhase::Sections;
    activeTopLevelTab = TopLevelTab::OnDevice;
    displayLocalLibrary(true, "settings-back");
}
unsigned deviceSettingsCount() {
    switch(wifiPage) {
        case WifiPage::Settings: return 8;
        case WifiPage::Reading: case WifiPage::Language: case WifiPage::Power: return 2;
        case WifiPage::Keyboard: return TextKeyboard::LayoutCount;
        case WifiPage::Display: return 3;
        case WifiPage::About: return 2;
        default: return 1;
    }
}
void paintDeviceSettings(ReaderBookishCanvas &c) {
    BookishUI::List v{}; v.activeTab=1;
    v.title=deviceText("Settings", "Настройки");
    v.subtitle=deviceText("Make yourself at home.", "Устройтесь поудобнее.");
    v.back=deviceText("< Back", "< Назад"); v.backFocused=wifiSelection==0;
    v.navigationHint=deviceText("Double OK — back · Hold OK — tabs", "Двойной OK — назад · Удержать — вкладки");
    const char *names[8]{}; const char *details[8]{}; char values[4][96]{};
    const auto &s=activeReaderSettings;
    if(wifiPage==WifiPage::Settings) {
        names[0]=deviceText("Reading","Чтение"); details[0]=deviceText("Text size and line spacing","Размер текста и интервал");
        names[1]=deviceText("Screen","Экран"); details[1]=deviceText("Refresh and clear the display","Обновление и очистка экрана");
        names[2]=deviceText("Sleep","Сон"); details[2]=deviceText("Automatic deep sleep","Автоматический глубокий сон");
        names[3]=deviceText("Language","Язык"); details[3]=s.language==I18n::Language::English ? "English" : "Русский";
        names[4]="Wi-Fi"; details[4]=deviceText("Choose a network and password","Выбор сети и ввод пароля");
        names[5]=deviceText("Library connection","Подключение к библиотеке"); details[5]=deviceText("Server and device pairing","Сервер и привязка устройства");
        names[6]=deviceText("About this reader","Об устройстве"); details[6]="AbyssBook";
        names[7]=deviceText("Keyboard languages","Языки клавиатуры"); details[7]=deviceText("Choose layouts for typing","Раскладки для ввода текста");
    } else if(wifiPage==WifiPage::Reading) {
        v.title=deviceText("Reading","Чтение");
        names[0]=deviceText("Text size","Размер текста");
        const char *sizes[]={deviceText("Small","Компактный"),deviceText("Medium","Обычный"),deviceText("Large","Крупный")};
        details[0]=sizes[static_cast<unsigned>(s.textSize)];
        names[1]=deviceText("Line spacing","Межстрочный интервал");
        const char *spacing[]={deviceText("Compact","Плотный"),deviceText("Normal","Обычный"),deviceText("Airy","Воздушный")};
        details[1]=spacing[static_cast<unsigned>(s.lineSpacing)];
        v.subtitle=deviceText("OK changes the value. Position is kept.","OK меняет значение. Позиция сохранится.");
    } else if(wifiPage==WifiPage::Display) {
        v.title=deviceText("Screen","Экран");
        names[0]=deviceText("Reading cleanup","Очистка при чтении");
        snprintf(values[0],sizeof(values[0]),deviceText("Every %u pages","Каждые %u страниц"),s.readingClearEvery); details[0]=values[0];
        names[1]=deviceText("Menu cleanup","Очистка меню");
        snprintf(values[1],sizeof(values[1]),deviceText("Every %u updates","Каждые %u обновления"),s.uiClearEvery); details[1]=values[1];
        names[2]=deviceText("Clear screen now","Очистить экран сейчас"); details[2]=deviceText("Remove traces of previous pages","Убрать следы предыдущих страниц");
    } else if(wifiPage==WifiPage::Power) {
        v.title=deviceText("Sleep","Сон"); names[0]=deviceText("Sleep after inactivity","Сон при бездействии");
        snprintf(values[0],sizeof(values[0]),deviceText("%u minutes","%u минут"),s.sleepMinutes); details[0]=values[0];
        v.subtitle=deviceText("Deep sleep. Wi-Fi stays off.","Глубокий сон. Wi-Fi выключен.");
        names[1]=deviceText("Sleep button type","Тип кнопки сна");
        details[1]=s.sleepLatching ? deviceText("Latching switch","С фиксацией") : deviceText("Momentary button","Без фиксации");
    } else if(wifiPage==WifiPage::Keyboard) {
        v.title=deviceText("Keyboard languages","Языки клавиатуры");
        v.subtitle=deviceText("Switch layouts with the EN / RU key.","Переключение — клавиша EN / RU при вводе.");
        for(unsigned i=0;i<TextKeyboard::LayoutCount;++i) {
            names[i]=TextKeyboard::layoutName(i);
            details[i]=i==0 ? deviceText("Always available","Всегда доступен") :
                (s.keyboardLayouts & (1U<<i)) ? deviceText("Enabled","Включён") : deviceText("Disabled","Выключен");
        }
    } else if(wifiPage==WifiPage::Language) {
        v.title=deviceText("Language","Язык"); names[0]="English"; names[1]="Русский";
        details[static_cast<unsigned>(s.language)]=deviceText("Selected","Выбран");
    } else if(wifiPage==WifiPage::Library) {
        v.title=deviceText("Library connection","Подключение");
        ProvisioningConfig config{}; char error[64]{};
        const bool bound=ProvisioningStore::load(config,error,sizeof(error));
        names[0]=bound ? deviceText("Library configured","Библиотека настроена") : deviceText("Pair with Companion","Привязать через Companion");
        if(bound) {
            const char *host=strstr(config.opdsUrl,"://"); host=host?host+3:config.opdsUrl;
            const size_t len=strcspn(host,"/"); snprintf(values[0],sizeof(values[0]),"%.*s",static_cast<int>(len),host); details[0]=values[0];
        } else details[0]=deviceText("Connect the reader to your PC by USB","Подключите читалку к ПК по USB");
        WifiCredentials::wipe(&config,sizeof(config));
        v.subtitle=deviceText("Manage pairing in Companion.","Управление привязкой в Companion.");
    } else {
        v.title=deviceText("About this reader","Об устройстве");
        names[0]="AbyssBook"; details[0]=kFirmwareVersion;
        names[1]=deviceText("Battery","Аккумулятор");
        snprintf(values[0],sizeof(values[0]),"%u%%",diagnostics.batteryPercent);details[1]=values[0];
    }
    v.total=deviceSettingsCount(); v.selected=wifiSelection ? wifiSelection-1:0;
    const unsigned start=(v.selected/BookishUI::kListRows)*BookishUI::kListRows;
    for(unsigned i=start;i<v.total && v.rowCount<BookishUI::kListRows;++i) {
        auto &r=v.rows[v.rowCount++];r.title=names[i];r.subtitle=details[i]?details[i]:"";r.selected=wifiSelection==i+1;
    }
    if(*wifiNotice)v.subtitle=wifiNotice;
    BookishUI::list(c,v);
}
bool inputDeviceSettings(const char *button,const char *gesture) {
    if(!deviceSettingsPage())return false;
    const bool ok=!strcmp(button,"CENTER"), back=ok && !strcmp(gesture,"DOUBLE");
    if(back || (ok && !strcmp(gesture,"SHORT") && wifiSelection==0)) {
        const bool exiting=wifiPage==WifiPage::Settings;deviceSettingsBack();
        if(!exiting)paintWifiSettings(); return true;
    }
    if(strcmp(gesture,"SHORT"))return true;
    if(!ok) {
        const unsigned n=deviceSettingsCount()+1;
        wifiSelection=(wifiSelection+(!strcmp(button,"UP")?n-1:1))%n;
    } else if(wifiPage==WifiPage::Settings) {
        const WifiPage pages[]={WifiPage::Settings,WifiPage::Reading,WifiPage::Display,WifiPage::Power,WifiPage::Language,WifiPage::Networks,WifiPage::Library,WifiPage::About,WifiPage::Keyboard};
        if(wifiSelection==5){wifiBeginScan();return true;}
        wifiSetPage(pages[wifiSelection]);wifiSelection=0;
    } else {
        ReaderSettings candidate=activeReaderSettings; bool changed=false;
        if(wifiPage==WifiPage::Reading) {
            if(wifiSelection==1)candidate.textSize=static_cast<ReaderTextSize>((static_cast<unsigned>(candidate.textSize)+1)%3);
            else candidate.lineSpacing=static_cast<ReaderLineSpacing>((static_cast<unsigned>(candidate.lineSpacing)+1)%3);
            changed=true;
        } else if(wifiPage==WifiPage::Display) {
            if(wifiSelection==1){candidate.readingClearEvery=candidate.readingClearEvery==8?12:candidate.readingClearEvery==12?24:8;changed=true;}
            else if(wifiSelection==2){candidate.uiClearEvery=candidate.uiClearEvery==1?2:candidate.uiClearEvery==2?4:1;changed=true;}
            else scheduleGhostCleanup("settings-clean");
        } else if(wifiPage==WifiPage::Power) {
            if(wifiSelection==1) candidate.sleepMinutes=candidate.sleepMinutes==5?15:candidate.sleepMinutes==15?30:candidate.sleepMinutes==30?60:5;
            else candidate.sleepLatching=!candidate.sleepLatching;
            changed=true;
        } else if(wifiPage==WifiPage::Keyboard && wifiSelection>1) {
            candidate.keyboardLayouts ^= 1U << (wifiSelection-1); changed=true;
        }
        else if(wifiPage==WifiPage::Language) {candidate.language=static_cast<I18n::Language>(wifiSelection-1);changed=true;}
        if(changed) {
            if(ReaderSettingsStore::save(candidate)) {
                activeReaderSettings=candidate;ReaderSettingsStore::apply(candidate);
                sleepSwitchLatching.store(candidate.sleepLatching);
                scheduleGhostCleanup("settings-changed");homeSession.loaded=false;localLibrarySession.loaded=false;favoritesSession.loaded=false;
            } else wifiNotice=deviceText("Could not save. Check your SD card.","Не сохранено. Проверьте SD-карту.");
        }
    }
    paintWifiSettings();return true;
}
