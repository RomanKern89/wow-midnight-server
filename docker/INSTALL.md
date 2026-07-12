# 📘 Full installation guide (beginner-friendly)

**EN** · [Русский ниже](#-полная-инструкция-по-установке-для-новичка-ru)

This guide takes you from a bare Ubuntu machine to a **fully running** WoW
Midnight server in containers. Every command is copy‑paste. No prior server
experience needed.

> You only supply three things that are Blizzard's property (we cannot ship
> them): your **game client data**, a **world** DB dump, and a **hotfixes** DB
> dump. Everything else is automated. See [DISCLAIMER](../DISCLAIMER.md).

---

## 1. System requirements

| | Minimum | Recommended | Notes |
|---|---|---|---|
| **OS** | Ubuntu 22.04 / 24.04 (64‑bit) | Ubuntu 24.04 | Any Docker host works; guide assumes Ubuntu |
| **CPU** | 4 cores | 6–8 cores | More cores = much faster first build |
| **RAM (building)** | 16 GB | 24 GB | The core compiles from source once |
| **RAM (running full world)** | **16 GB** | **20 GB** | A full retail world (~734k spawns + maps) uses ~14 GB; **8 GB crashes with OOM** |
| **Disk (free)** | 60 GB | 120 GB | Image ~7 GB + client data ~25 GB + databases ~2 GB + working room |
| **Software** | Docker Engine + Docker Compose v2 | latest | Installed in step 2 |
| **Network ports** | 1119, 8081, 8085 | + 3306 | Must be reachable by your game client |

> **Login‑server‑only mode** (no worldserver) runs comfortably in ~2 GB RAM and
> ~10 GB disk — handy for just testing the stack.

You also need, on any Windows PC: a **retail World of Warcraft client, build
12.0.7.68275**, to extract data from and to connect with.

Time: ~1 hour (mostly the one‑time build + data copy).

---

## 2. Install Docker + Docker Compose (on the Ubuntu machine)

Open a terminal and run, one block at a time:

```bash
# update the system
sudo apt-get update && sudo apt-get -y upgrade

# install Docker (official convenience script)
curl -fsSL https://get.docker.com | sudo sh

# let your user run docker without sudo (log out/in after this, or run: newgrp docker)
sudo usermod -aG docker "$USER"

# also install git + rsync (used later)
sudo apt-get install -y git rsync
```

Verify Docker works:

```bash
docker run --rm hello-world      # should print "Hello from Docker!"
docker compose version           # should print Docker Compose version v2.x
```

---

## 3. Get this repository

```bash
cd ~
git clone -b docker https://github.com/RomanKern89/wow-midnight-server.git
cd wow-midnight-server/docker
```

---

## 4. Prepare your game data (the Blizzard‑owned parts)

You need **three** things from your own legal client + an existing TrinityCore
install. If you already run a TrinityCore server, this is quick.

### 4a. Client‑extracted data → `data/`

On the PC with your WoW client (build 68275), run the TrinityCore extractor
tools (`mapextractor`, `vmap4extractor` + `vmap4assembler`, `mmaps_generator`)
inside the client folder. They produce these folders:

```
dbc/  maps/  vmaps/  mmaps/  cameras/  gt/
```

Copy them into the repo's `docker/data/` folder. When done, `data/` looks like:

```
docker/data/
├── dbc/     maps/     vmaps/     mmaps/     cameras/     gt/
```

> This is ~25 GB. If you already have these on a TrinityCore server, just copy
> that server's `data` directory here (e.g. with `scp` or `rsync`).

### 4b. Database dumps → `import/`

From an existing TrinityCore server (or however you build your DBs), export the
**world** and **hotfixes** databases (build 68275):

```bash
mysqldump --single-transaction --no-tablespaces --routines -u<user> -p world    > world.sql
mysqldump --single-transaction --no-tablespaces --routines -u<user> -p hotfixes > hotfixes.sql
```

Put both files into the repo's `docker/import/` folder:

```
docker/import/
├── world.sql        # quests, spawns, loot, vendors
└── hotfixes.sql     # REQUIRED — worldserver crash-loops without it
```

> `--single-transaction` makes the export safe on a live server (no locking).
> `data/` and `import/` are git‑ignored, so you can never accidentally commit
> Blizzard data.

---

## 5. Configure

```bash
cp .env.example .env
nano .env        # edit the values (Ctrl+O to save, Ctrl+X to exit)
```

Set at least:

```ini
MYSQL_ROOT_PASSWORD=choose-a-strong-password
DB_PASS=choose-another-password
# The IP your game client will connect to (this machine's LAN or public IP):
REALM_ADDRESS=192.168.1.50
LOGIN_REST_EXTERNAL_ADDRESS=192.168.1.50
```

Find this machine's IP with: `hostname -I`.

---

## 6. Build and launch

```bash
docker compose up -d --build
```

- The **first** run compiles TrinityCore from source — **~30 minutes**. Grab a
  coffee. Later starts are instant.
- Then it starts MySQL, imports your databases + applies our fixes, generates a
  login certificate, and boots the world.

Watch progress:

```bash
docker compose logs -f db-init        # database import (Ctrl+C to stop watching)
docker compose logs -f worldserver    # world boot; wait for "World initialized in ..."
```

When you see **`World initialized in X minutes`**, the server is up.

Check all services:

```bash
docker compose ps
# tc-mysql        Up (healthy)
# tc-bnetserver   Up
# tc-worldserver  Up
```

---

## 7. Create a game account

The worldserver has an interactive console:

```bash
docker attach tc-worldserver
```

Type (then press Enter):

```
account create myname mypassword
account set gmlevel myname 3 -1
```

Detach **without stopping** the server with: **Ctrl‑P** then **Ctrl‑Q**.

---

## 8. Connect your client

On the Windows PC with the client (build 68275):

1. Put the **Arctium** launcher next to `WoW.exe`.
2. Edit `WoW/_retail_/WTF/Config.wtf`, add one line:
   `SET portal "192.168.1.50"` (your `REALM_ADDRESS` from step 5).
3. Launch through **Arctium**, log in with the account from step 7.

Full client details + login troubleshooting: [../docs/en/CONNECT.md](../docs/en/CONNECT.md).

---

## Verify everything works

```bash
# services all "Up", worldserver not restarting:
docker compose ps
# ports listening:
sudo ss -tln | grep -E ':(1119|8081|8085|3306)'
# worldserver healthy (restarts should be 0):
docker inspect --format '{{.State.Status}} restarts={{.RestartCount}} oom={{.State.OOMKilled}}' tc-worldserver
```

---

## Managing the server

```bash
docker compose stop           # stop everything (keeps data)
docker compose start          # start again
docker compose restart worldserver
docker compose logs -f worldserver
docker compose down           # remove containers (keeps DB volume + your data)
docker compose down -v        # ALSO wipe the database volume (fresh start)

# update to the latest fixes:
git pull
docker compose up -d --build
```

---

## Troubleshooting

| Symptom | Cause & fix |
|---|---|
| `db-init` says **"No world DB dump found"** | Put `world.sql` in `docker/import/`, then `docker compose up db-init` |
| worldserver log: **"Table 'hotfixes.… doesn't exist"**, crash‑loops | Missing `hotfixes.sql` in `docker/import/` — add it and re‑run `docker compose up db-init` |
| worldserver: **"missing maps"**, exits | Client data not in `docker/data/` (need `dbc/maps/vmaps/mmaps`) |
| worldserver restarts, `OOMKilled=true` | Not enough RAM — give the machine **16–20 GB** |
| Build fails / killed | Not enough RAM to compile (need 16 GB+), or lower `BUILD_JOBS` in `.env` |
| Client stuck on "Connecting…" | `LOGIN_REST_EXTERNAL_ADDRESS` wrong, or port 8081 blocked; check `SET portal` |
| "Version mismatch" at login | Client build must be exactly **68275** |

> The world DB import errors 1419 / 1227 that older setups hit are already
> handled by this compose (mysql `log-bin-trust-function-creators=1`, and dumps
> imported as root with DEFINER stripped).

---
---

# 📘 Полная инструкция по установке для новичка (RU)

[English above](#-full-installation-guide-beginner-friendly)

Инструкция проведёт от «пустой машины с Ubuntu» до **полностью работающего**
сервера WoW Midnight в контейнерах. Все команды — копипаст. Опыт администрирования
не нужен.

> Ты предоставляешь только три вещи, принадлежащие Blizzard (мы их не
> распространяем): **данные игрового клиента**, дамп БД **world** и дамп БД
> **hotfixes**. Всё остальное автоматизировано. См. [DISCLAIMER](../DISCLAIMER.md).

---

## 1. Системные требования

| | Минимум | Рекомендуется | Примечание |
|---|---|---|---|
| **ОС** | Ubuntu 22.04 / 24.04 (64‑бит) | Ubuntu 24.04 | Подойдёт любой Docker‑хост; инструкция под Ubuntu |
| **CPU** | 4 ядра | 6–8 ядер | Больше ядер = быстрее первая сборка |
| **RAM (сборка)** | 16 ГБ | 24 ГБ | Ядро один раз компилируется из исходников |
| **RAM (полный мир)** | **16 ГБ** | **20 ГБ** | Полный ретейл‑мир (~734к спавнов + карты) ест ~14 ГБ; **8 ГБ падает от OOM** |
| **Диск (свободно)** | 60 ГБ | 120 ГБ | Образ ~7 ГБ + данные клиента ~25 ГБ + базы ~2 ГБ + запас |
| **ПО** | Docker Engine + Docker Compose v2 | последняя | Ставится в шаге 2 |
| **Порты** | 1119, 8081, 8085 | + 3306 | Должны быть доступны игровому клиенту |

> **Режим «только логин‑сервер»** (без worldserver) работает в ~2 ГБ RAM и
> ~10 ГБ диска — удобно просто проверить стек.

Также нужен на любом ПК с Windows: **ретейл‑клиент World of Warcraft билда
12.0.7.68275** — из него извлекаешь данные и им же подключаешься.

Время: ~1 час (в основном разовая сборка + копирование данных).

---

## 2. Установка Docker + Docker Compose (на машине с Ubuntu)

Открой терминал и выполняй по одному блоку:

```bash
# обновить систему
sudo apt-get update && sudo apt-get -y upgrade

# установить Docker (официальный скрипт)
curl -fsSL https://get.docker.com | sudo sh

# разрешить запуск docker без sudo (после этого перелогинься, или выполни: newgrp docker)
sudo usermod -aG docker "$USER"

# заодно git + rsync (понадобятся позже)
sudo apt-get install -y git rsync
```

Проверить, что Docker работает:

```bash
docker run --rm hello-world      # должно вывести "Hello from Docker!"
docker compose version           # должно вывести Docker Compose version v2.x
```

---

## 3. Скачать репозиторий

```bash
cd ~
git clone -b docker https://github.com/RomanKern89/wow-midnight-server.git
cd wow-midnight-server/docker
```

---

## 4. Подготовить игровые данные (части Blizzard)

Нужны **три** вещи из твоего легального клиента + существующей установки
TrinityCore. Если у тебя уже есть TrinityCore‑сервер — это быстро.

### 4a. Данные клиента → `data/`

На ПК с клиентом WoW (билд 68275) запусти инструменты‑экстракторы TrinityCore
(`mapextractor`, `vmap4extractor` + `vmap4assembler`, `mmaps_generator`) внутри
папки клиента. Они создают папки:

```
dbc/  maps/  vmaps/  mmaps/  cameras/  gt/
```

Скопируй их в папку репозитория `docker/data/`. В итоге `data/` выглядит так:

```
docker/data/
├── dbc/     maps/     vmaps/     mmaps/     cameras/     gt/
```

> Это ~25 ГБ. Если данные уже есть на TrinityCore‑сервере — просто скопируй
> оттуда папку `data` (например через `scp`/`rsync`).

### 4b. Дампы баз → `import/`

С существующего TrinityCore‑сервера выгрузи базы **world** и **hotfixes**
(билд 68275):

```bash
mysqldump --single-transaction --no-tablespaces --routines -u<user> -p world    > world.sql
mysqldump --single-transaction --no-tablespaces --routines -u<user> -p hotfixes > hotfixes.sql
```

Положи оба файла в `docker/import/`:

```
docker/import/
├── world.sql        # квесты, спавны, лут, вендоры
└── hotfixes.sql     # ОБЯЗАТЕЛЬНО — без него worldserver крашлупит
```

> `--single-transaction` делает выгрузку безопасной на живом сервере (без
> блокировок). `data/` и `import/` в .gitignore — случайно не закоммитишь.

---

## 5. Настройка

```bash
cp .env.example .env
nano .env        # правишь значения (Ctrl+O сохранить, Ctrl+X выйти)
```

Задай минимум:

```ini
MYSQL_ROOT_PASSWORD=надёжный-пароль
DB_PASS=другой-пароль
# IP, к которому будет подключаться клиент (LAN или публичный IP этой машины):
REALM_ADDRESS=192.168.1.50
LOGIN_REST_EXTERNAL_ADDRESS=192.168.1.50
```

Узнать IP машины: `hostname -I`.

---

## 6. Сборка и запуск

```bash
docker compose up -d --build
```

- **Первый** запуск компилирует TrinityCore из исходников — **~30 минут**.
  Дальнейшие старты — мгновенные.
- Затем стартует MySQL, импортирует твои базы + применяет наши фиксы, генерит
  логин‑сертификат и грузит мир.

Смотреть прогресс:

```bash
docker compose logs -f db-init        # импорт баз (Ctrl+C — прекратить просмотр)
docker compose logs -f worldserver    # загрузка мира; жди "World initialized in ..."
```

Когда увидишь **`World initialized in X minutes`** — сервер поднят.

Проверить сервисы:

```bash
docker compose ps
# tc-mysql        Up (healthy)
# tc-bnetserver   Up
# tc-worldserver  Up
```

---

## 7. Создать игровой аккаунт

У worldserver интерактивная консоль:

```bash
docker attach tc-worldserver
```

Введи (с Enter):

```
account create myname mypassword
account set gmlevel myname 3 -1
```

Выйти **не останавливая** сервер: **Ctrl‑P**, затем **Ctrl‑Q**.

---

## 8. Подключить клиент

На ПК с клиентом (билд 68275):

1. Положи лаунчер **Arctium** рядом с `WoW.exe`.
2. В `WoW/_retail_/WTF/Config.wtf` добавь строку:
   `SET portal "192.168.1.50"` (твой `REALM_ADDRESS` из шага 5).
3. Запускай через **Arctium**, входи аккаунтом из шага 7.

Подробности клиента + диагностика логина: [../docs/ru/CONNECT.md](../docs/ru/CONNECT.md).

---

## Проверка, что всё работает

```bash
# все сервисы "Up", worldserver не рестартится:
docker compose ps
# порты слушают:
sudo ss -tln | grep -E ':(1119|8081|8085|3306)'
# worldserver здоров (restarts = 0):
docker inspect --format '{{.State.Status}} restarts={{.RestartCount}} oom={{.State.OOMKilled}}' tc-worldserver
```

---

## Управление сервером

```bash
docker compose stop           # остановить всё (данные сохраняются)
docker compose start          # запустить снова
docker compose restart worldserver
docker compose logs -f worldserver
docker compose down           # удалить контейнеры (том БД + данные остаются)
docker compose down -v        # ТАКЖЕ стереть том БД (с чистого листа)

# обновиться до свежих фиксов:
git pull
docker compose up -d --build
```

---

## Решение проблем

| Симптом | Причина и решение |
|---|---|
| `db-init`: **"No world DB dump found"** | Положи `world.sql` в `docker/import/`, затем `docker compose up db-init` |
| worldserver: **"Table 'hotfixes.… doesn't exist"**, крашлуп | Нет `hotfixes.sql` в `docker/import/` — добавь и повтори `docker compose up db-init` |
| worldserver: **"missing maps"**, выходит | Нет данных клиента в `docker/data/` (нужны `dbc/maps/vmaps/mmaps`) |
| worldserver рестартится, `OOMKilled=true` | Мало RAM — дай машине **16–20 ГБ** |
| Сборка падает / убита | Мало RAM для компиляции (нужно 16 ГБ+), или уменьши `BUILD_JOBS` в `.env` |
| Клиент висит на "Connecting…" | Неверный `LOGIN_REST_EXTERNAL_ADDRESS` или закрыт порт 8081; проверь `SET portal` |
| "Version mismatch" на логине | Билд клиента должен быть ровно **68275** |

> Ошибки импорта world 1419 / 1227, на которые натыкались старые сборки, уже
> обрабатываются этим compose (mysql `log-bin-trust-function-creators=1`, дампы
> импортируются под root со срезанным DEFINER).
