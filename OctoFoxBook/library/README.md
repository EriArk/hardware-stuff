# OctoFox Library

Личная веб-библиотека: загрузка FB2 и EPUB, поиск, коллекции, чтение, закладки и серверная озвучка. Открывается в браузере на компьютере, планшете и телефоне.

**Ранняя версия для отдельной тестовой установки.** Работают библиотека, первоначальная настройка и управление читателями через Companion. Companion также сохраняет сетевые адреса, помогает настроить внешний доступ и проверяет подключение через QR-код. Интерфейс доступен на русском и английском. Привязка AbyssBook реализована в текущих исходниках; проверка USB на физической читалке ещё нужна. Аккаунты и общий каталог обслуживает BookLore.

## Ранняя бета

[OctoFox Book 0.2.0-beta.4 — скачать сборки](https://github.com/EriArk/hardware-stuff/releases/tag/octofox-v0.2.0-beta.4). Установщик сервера и переносимый браузер Companion доступны отдельно. Проверенные сценарии и ограничения указаны в описании релиза.

## Установка через мастер

[OctoFox Server Setup](../server-installer/) — отдельный установщик сервера с графической страницей: проверяет компьютер, подготавливает недостающие зависимости и запускает библиотеку, озвучку и серверный Companion. Программа-браузер Companion остаётся отдельной и запускается без установки. Ниже сохранены команды для ручного развёртывания и администрирования.

## Установка

Нужны Python 3.12+ и работающий Docker Engine с Compose 2.20+ (на Windows — Docker Desktop в режиме Linux containers). Запускайте команды на компьютере с Docker, используя локальный Docker context. Сборка озвучки загружает модели и зависимости, поэтому требует Интернета и нескольких гигабайт свободного места. После сборки голосовой контейнер работает без сети.

Из этой папки:

```sh
python tools/manage.py check
python tools/manage.py start
```

Companion — панель управления, уже входящая в книжный сервер. Для доступа с ПК используется отдельное приложение без установки: оно находит сервер и отображает его панель в своём окне. Установка самого сервера выполняется отдельно.

Теперь откройте [переносимое приложение OctoFox Companion](../companion/) на этом компьютере или в его локальной сети. Оно само найдёт сервер и откроет форму создания владельца. Введите имя, email, будущие логин и пароль — ключ настройки и ввод IP не нужны. После настройки можно создавать читателей и загружать FB2/EPUB; переход к чтению не требует повторного входа. Email используется в профиле, приглашения автоматически не отправляются.

`start` создаёт `.env`, только если его ещё нет, проверяет порты и ждёт готовности сервисов. Повторный запуск сохраняет настройки, книги и ранее включённую озвучку. Состояние можно посмотреть через `python tools/manage.py status`, остановить установку с сохранением данных — через `python tools/manage.py stop`. Для повторного запуска без сборки используйте `start --no-build`.

Для отдельной установки с другим именем проекта добавляйте `--project имя` **перед** командой, например `python tools/manage.py --project my-library status`. Используйте это имя при каждом обращении к установке. Запускатор откажется управлять одноимённым проектом из другой папки. Продвинутый вариант с `tools/configure.py` и обычными командами Compose также остаётся доступен.

Новую установку можно настроить только один раз через приложение в локальной сети. На публичной веб-странице вместо формы владельца предлагается открыть Companion. Если создание аккаунта прошло, а подключение к чтению прервалось, войдите в Companion и используйте «Завершить подключение» с логином и паролем этого аккаунта. Повторно создавать его не нужно. Существующие пароли Companion автоматически не меняет.

В списке аккаунтов нажмите «Управлять»: здесь можно изменить имя и email, задать новый пароль или временно отключить читателя. При отключении сеансы OctoFox закрываются, вход запрещается, но книги, коллекции и место чтения сохраняются. Доступ администратора отключить нельзя.

Смена пароля обновляет основной пароль аккаунта и одноимённый доступ к чтению. Старые сеансы OctoFox завершаются; после изменения собственного пароля администратор входит снова. Если операция прервалась, аккаунт помечается в списке: повторите смену пароля через «Управлять». До завершения чтение закрыто, книги остаются на месте. Эти действия относятся к основному логину OctoFox. Отдельные OPDS-логины, созданные вручную в BookLore, управляются там же; отключение OctoFox не блокирует прямой вход в BookLore.

Новые читатели получают личные загрузки и чтение; административных прав и доступа к чужим каталогам у них нет. Управление общими каталогами и их назначение пользователям пока выполняются в BookLore на `http://localhost:8081`. При необходимости добавьте там каталог `/books`. Если BookLore уже настроен отдельно, войдите в Companion его административным аккаунтом; существующие OPDS-логины библиотеки продолжают работать.

На удалённом хосте административный порт доступен через SSH-туннель:

```sh
ssh -L 8081:127.0.0.1:8081 user@server
```

Чтобы открыть Companion удалённой установки, добавьте отдельный проброс: `ssh -L 8080:127.0.0.1:8080 user@server`, затем откройте `http://localhost:8080/companion` на своём компьютере. При изменённых портах подставьте свои значения.

Новые установки по умолчанию доступны в локальной сети; приложение автоматически разрешает найденный LAN-адрес после входа владельца. Для явного выбора адреса перед первым запуском:

```sh
python tools/manage.py start --origin http://192.168.1.20:8080 --bind 0.0.0.0
```

Адрес приведён для примера. Для работы только на своём компьютере используйте `--bind 127.0.0.1`. Обнаружение использует UDP 49645, который нужно разрешить в частной сети вместе с HTTP-портом библиотеки. Основной адрес указан в `OCTOFOX_ORIGIN`; дополнительные адреса можно добавить в Companion. Генератор не перезаписывает существующий `.env`: сохраняйте его вместе с резервной копией. Для публикации через HTTPS задайте соответствующий origin и обратный прокси. Сам по себе запуск Compose не настраивает внешний доступ или Cloudflare.

Параметры `--origin`, `--bind`, `--port` и `--admin-port` предназначены только для первой настройки. Если `.env` уже существует, измените нужные строки в нём и снова выполните `python tools/manage.py start`. Ошибка готовности не удаляет данные; `status` покажет сервис, который ещё не готов. Если Docker недоступен, сначала запустите Docker Engine/Desktop.

## Подключение с других устройств

В Companion откройте «Подключение». Сохраните IP сервера в домашней сети и/или свой HTTPS-домен — по одному адресу в строке. Дополнительные адреса сохраняются между перезапусками; основной адрес установки всегда остаётся доступен. Разрешение адреса вступает в силу сразу, но не открывает порт и не настраивает DNS.

Если установка слушает только `127.0.0.1`, для домашней сети измените `OCTOFOX_BIND_ADDRESS=0.0.0.0` в `.env`, разрешите порт библиотеки в брандмауэре для домашней сети и выполните:

```sh
docker compose up -d --no-deps library
```

Кнопка «Определить внешний IP» обращается с сервера к [ipify](https://www.ipify.org/) только по нажатию. Найденный IP не доказывает доступность извне: возможны CGNAT, VPN или закрытый порт. В разделе есть пошаговая помощь для [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/get-started/create-remote-tunnel/). Коннектор устанавливается на сервере; Companion не запрашивает токен Cloudflare и не перенастраивает роутер.

В **Connect through Cloudflare → Open illustrated guide** доступна английская инструкция с пятью иллюстрированными шагами: домен, туннель, маршрут, разрешение адреса и проверка с телефона. Укажите имя домена и расположение коннектора — примеры покажут ваш адрес и порт. **Add address to form** дополняет форму, сохраняя прежние адреса; затем нужно нажать «Сохранить адреса». Иллюстрации — схемы с пояснениями, а не снимки панели Cloudflare. Перевод этой инструкции планируется позже.

Для проверки выберите сохранённый внешний адрес и нажмите «Создать QR-код проверки». Выключите Wi-Fi на телефоне, оставив мобильный Интернет, и отсканируйте код камерой. Вход в аккаунт не нужен. Телефон покажет подтверждение, а Companion автоматически обновит результат. QR-код и ссылка действуют 10 минут, создаются внутри вашей установки и не дают доступа к книгам или аккаунту. Сетевые адреса вроде `192.168.*` и `localhost` не подходят для проверки через мобильный Интернет.

Отметка подтверждает запрос из браузера по выбранному адресу. Companion не может сам определить, был ли телефон в Wi-Fi: это проверяет пользователь. Перезапуск сервиса завершает проверки и административные сеансы, но сохраняет адреса и книги.

## Синхронизация читалки

В разделе «Моя читалка» у каждого устройства есть галочка **«Синхронизировать весь профиль»**. По умолчанию она включена: при нажатии «Синхронизация» на читалке загружаются «Мои книги», избранное и книги из личных коллекций. Весь общий каталог не скачивается. Снятая галочка возвращает ручной выбор книг для устройства.

Избранное и коллекции доступны на сайте и в прошивке AbyssBook 0.21.0-alpha6. Коллекцию можно создать на читалке; изменения отправляются при следующей синхронизации. Раздел «Избранное» сайта позволяет переключаться между избранными книгами и коллекциями. Если книга больше не входит в синхронизируемый профиль, её копия на читалке переносится в локальный архив; серверный файл не удаляется. Книги, добавленные только по USB, автоматически не загружаются на сервер.

В исходниках alpha7 добавлен двусторонний обмен местом чтения, отметкой «прочитано» и закладками. Позиции привязаны к тексту, а не к номеру страницы. Если чтение продолжалось независимо на сайте и устройстве, текущей становится позиция читалки, а прежняя позиция сайта сохраняется закладкой. Несовместимая копия книги или неподдерживаемая позиция останавливает обмен вместо приблизительного переноса. Проверены реальная USB-привязка в Windows, скачивание книги, обмен позицией в обе стороны и перенос закладки с сайта на физическую читалку. Полная пользовательская приёмка продолжается.

В обновлённом настольном Companion откройте **Pair reader**, подключите читалку по USB, выберите аккаунт и настроенный HTTPS-адрес библиотеки. Для текущего аккаунта владельца повторный пароль не нужен; пароль другого читателя вводится однократно. На устройство записывается отдельный отзывной ключ, а не пароль аккаунта. Wi-Fi сохраняется; выбрать сеть можно в настройках читалки. Ключ отзывается в том же разделе Companion. После привязки обмен работает через Интернет без запущенного Companion. Радио включается только по явному действию пользователя. Используйте beta.4 и прошивку alpha8: исправлены обнаружение USB и ссылки скачивания через публичный домен.

## Язык интерфейса

Library и Companion доступны на русском и английском. При первом открытии используется предпочитаемый поддерживаемый язык браузера; переключатель Language на экране входа, в меню библиотеки и в Companion сохраняет выбор для этой установки.

Переведены элементы управления, настройки чтения и озвучки, формы, сетевые инструкции, QR-проверка, сообщения об ошибках и встроенные жанры. Названия книг, авторы, текст, теги и собственные имена коллекций сохраняются в оригинале. Поиск жанров и алфавит соответствуют выбранному языку интерфейса.

## Озвучка

```sh
python tools/manage.py start --speech
```

Доступны семь серверных голосов для шести языков: русский, английский (США), немецкий, французский, испанский (Испания) и португальский (Бразилия). Выбор голоса и скорость доступны в читалке. Выберите голос под язык книги; смена языка интерфейса не меняет язык книги или голос. Модели загружаются из источников разработчиков с проверкой контрольных сумм; в Git они не хранятся. См. [speech/README.md](speech/README.md).

## Backups and restore

The current source includes **Companion → Backups**, currently in English. Included in the `0.2.0-beta.4` server installers. The original `0.2.0-beta.1` packages do not include backups. Build the current server source with `python tools/manage.py start` using the installation's existing project name and `.env`. The portable Companion browser does not need an update.

1. Sign in to Companion as an administrator and open **Backups**.
2. Choose **Create backup**. The library briefly pauses so account data, books and reading state belong to the same snapshot. Keep the server running. The page reconnects when it returns.
3. Sign in again and choose **Download ZIP**. Keep a copy on another device; a backup kept only on the server does not protect against losing that server.
4. To restore, choose the ZIP and **Upload and check**. File checksums and compatibility are checked while the library keeps running.
5. Review the date and size, type `RESTORE`, and choose **Create safety copy and restore**. This replaces all library users' data. A full safety archive is saved first. Afterward, sign in with an administrator account from the restored backup.

For a new server, install the current matching server version, create a temporary owner through Companion, then restore the backup. The temporary owner is replaced by the restored accounts. The destination keeps its database connection credentials, ports and discovery identity. Saved additional library addresses travel with the backup; review **Connection** after a move. The destination's primary installation address remains available.

Included: uploaded FB2/EPUB content, the shared BookLore catalogue and its files, account/password records, access controls, collections, favorites, bookmarks, reading positions, and library settings. Generated audio is excluded and is recreated on demand. Host `.env`, Cloudflare tunnel credentials, operating-system firewall configuration, downloaded archives and setup/discovery identity are not copied. Keep host configuration separately if you need to reproduce the same network deployment.

Archives are ZIP files with a versioned manifest and SHA-256 for every file; **they are not encrypted**. Only administrators can create, upload, restore, download or delete them. Use archives you made or trust. Maximum archive size is 20 GB, expanded data 200 GB / 200,000 files; up to 64 server copies are retained until you delete them. Unused uploads expire after 24 hours. Restore currently requires matching pinned BookLore and MariaDB images; it is not a version migration tool or an automatic update mechanism.

If a restore fails, the worker attempts to restore the safety copy before reopening the library. The operation journal survives worker restarts. If the page reports **Recovery needed**, retain all volumes and archives, check disk space and Docker, then run `docker compose restart backups` from the installation folder (include the existing `-p PROJECT` if applicable). Do not remove the safety archive. A permanent storage or Docker failure still requires operator repair.

### Backup service access

Compose adds a `backups` maintenance service and the private `backup-data` / `backup-control` volumes. The worker has no network, no published port and a read-only root filesystem. Its private Unix socket accepts fixed operations for the same Compose project; it verifies the data volume identities before stopping services. The web container does not mount the Docker socket.

The worker itself mounts `/var/run/docker.sock`, which grants powerful access to the local Docker daemon. This is required by this implementation to pause/restart the library and BookLore and export/import their account database. Restrict administration and access to the Compose files, volumes and worker image accordingly. This setup assumes the standard local Linux-container Docker socket; rootless/custom socket layouts and Docker Desktop Enhanced Container Isolation need separate configuration/verification. Without the worker (for example a direct Python development run), Companion shows that backups are unavailable.

## Данные и обновления

Книги пользователей, состояние чтения, база BookLore и аудиокэш находятся в именованных Docker-томах. Удаление книги из личной полки не стирает её позицию чтения. Перед обновлением сохраните и скачайте копию через Companion. Для ручного полного снимка томов остановите **эту** установку и сохраните её тома и `.env`. Команда `docker compose down` сохраняет тома; не используйте `down -v`, если данные нужны.

Не подключайте эту раннюю сборку к каталогам данных другой работающей установки. При замене личного файла книги прогресс сохраняется, но изменённое издание может иметь другой текст в той же позиции.

## Запуск без Docker

Для разработки веб-части достаточно Python 3.12; внешних runtime-зависимостей нет:

```sh
python -m pip install -e .
octofox-library
```

При прямом запуске Python также работает встроенное обнаружение; HTTP-порт должен совпадать с `OCTOFOX_PUBLISHED_PORT`. Настройте переменные окружения `OCTOFOX_UPSTREAM` (адрес BookLore с `/api/v1/opds`), `OCTOFOX_ORIGIN`, `OCTOFOX_DB`, `OCTOFOX_BIND`, `OCTOFOX_PORT`. Без подключённого BookLore войти нельзя. Ключ Companion создаётся рядом с `OCTOFOX_DB` в файле `companion-setup-key`; не публикуйте его. Административная сессия действует час и завершается при перезапуске веб-сервиса. Пароли и токены аккаунтов не сохраняются на диск. `OCTOFOX_SPEECH_SOCKET` включает серверную озвучку через Unix-сокет; без него веб-чтение остаётся доступным.

## Проверки

```sh
python -m pip install -e .
python -m unittest discover -s tests
node --test tests/test_books_web_*.cjs
```

Тесты используют временные данные и синтетические книги. Они не обращаются к личной библиотеке. Проверки реального аудио и поведения мобильного браузера выполняются отдельно.

Для английского варианта браузерных проверок задайте `QA_LANGUAGE=en`; локализованные файлы готовятся тем же кодом, что и на сервере. `PYTHON` позволяет указать путь к Python.

При установленном Playwright и его Chromium/WebKit можно выполнить проверку адаптивной разметки: `node tools/verify-browser.cjs`. `TEST_WEBKIT=1` выбирает WebKit; `QA_OUTPUT` задаёт папку снимков, по умолчанию `test-results/browser`. Проверка использует локальный HTTP-стенд с вымышленными книгами. Это не проверка физического iPhone.

`node tools/verify-companion.cjs` проверяет формы и адаптивную разметку Companion на синтетическом стенде; те же переменные выбирают браузер и папку снимков. Для генерации настоящего QR-кода используется Python (путь можно задать через `PYTHON`). Проверяются также сохранение адресов, QR-код, подтверждение и истечение срока проверки. Реальные аккаунты не создаются.

На Linux контейнерная проверка backend выполняется без внешней сети:

```sh
docker run --rm --network none -e PYTHONPATH=/work/src -v "$PWD:/work:ro" -w /work octofox-library:0.1.0-alpha1 python -m unittest discover -s tests
```

Проверка всех семи настоящих голосов после сборки:

```sh
docker run --rm --network none --memory 2g --cpus 1 -v "$PWD/speech/verify.py:/app/verify.py:ro" octofox-speech:0.1.0-alpha1 python /app/verify.py
```
