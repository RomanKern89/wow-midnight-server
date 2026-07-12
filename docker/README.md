# 🐳 Docker Compose deployment

**EN** · [Русский ниже](#-развёртывание-через-docker-compose-ru)

Run the whole stack — MySQL, worldserver, bnetserver — in containers. The core
is built from TrinityCore source inside the image, our community fixes are
applied automatically, and you supply the two Blizzard-owned inputs yourself.

> This is the **containerized** path. Prefer a manual install? See the
> [main branch](https://github.com/RomanKern89/wow-midnight-server) — both ways
> lead to the same server.

## ✅ Verified from scratch

Tested end-to-end on a **clean Ubuntu 24.04 VM** (`docker compose down -v --rmi
local` → `up -d --build` from zero):

- **Image builds from source** — compiles TrinityCore master (~30 min), 0 errors, image ~7 GB.
- **MySQL** → `healthy`; **db-init** → creates `auth`/`characters`/`world`/`hotfixes`, imports base schema (auth 39 + characters 132 tables), applies our fixes, exits `0` — clean log, no warnings.
- **bnetserver** → generates a self-signed dev cert and runs; ports **1119 + 8081 + 3306** listening.
- **worldserver** → with real data provided (world + hotfixes dumps + client data, on a 20 GB VM) it **boots fully**: *"World initialized in 4 minutes 12 seconds"*, all four ports (1119/8081/8085/3306) live, 0 restarts. Without your data it reaches the data gate and waits — a Blizzard-data requirement, **not** a bug.

## What you provide (Blizzard's property — not shipped)

| Input | Where | Notes |
|-------|-------|-------|
| Client-extracted data (dbc/maps/vmaps/mmaps) | `docker/data/` | From your own retail client (build 68275) |
| `world.sql` dump @68275 | `docker/import/` | quests/spawns/loot (+ our fixes on top) |
| `hotfixes.sql` dump @68275 | `docker/import/` | **required** — worldserver crash-loops without it |

Everything else — schema creation, base auth/character import, **our fixes**,
config rendering, a self-signed login cert — is automatic. See
[import/README.md](import/README.md) for how to produce the dumps.

## Requirements

- Docker Engine + Docker Compose v2
- **Build:** 24 GB+ RAM and time (the core compiles from source, ~30 min)
- **Run (full worldserver):** **16–20 GB RAM** — a full retail world (≈734k
  spawns + maps) OOM-kills at 8 GB. MySQL + bnetserver alone run in ~2 GB.
- Ports free: `8085` (world), `1119` (bnet), `8081` (REST login), `3306` (MySQL)

## Steps

```bash
cd docker
cp .env.example .env
# edit .env: set passwords + REALM_ADDRESS / LOGIN_REST_EXTERNAL_ADDRESS
#            to the IP your client will connect to.

# 1) put your world dump into ./import/  and your client data into ./data/

# 2) build + start
docker compose up -d --build      # first build is long (compiles TrinityCore)

# 3) watch it come up
docker compose logs -f db-init        # DB import + fixes
docker compose logs -f worldserver    # first boot builds caches (~minutes)
```

Create a game account (worldserver has an interactive console):

```bash
docker attach tc-worldserver          # detach with Ctrl-P Ctrl-Q
# in the console:
account create myname mypassword
account set gmlevel myname 3 -1
```

Then point your client at the host — see
[../docs/en/CONNECT.md](../docs/en/CONNECT.md).

### Two run states
- **Without your data** (empty `import/` + `data/`): MySQL, DB bootstrap and the
  **bnetserver login server come up** — useful to validate the stack. `worldserver`
  will restart-loop (no maps / empty world DB).
- **With your data** (world dump in `import/`, client data in `data/`):
  `worldserver` boots fully and you get an in-game-ready realm.

## Common issues

| Symptom | Fix |
|---------|-----|
| `db-init` says "No world DB dump found" | Put a `.sql` into `docker/import/` and `docker compose up db-init` |
| worldserver exits: missing maps | Mount client data into `docker/data/` (dbc/maps/vmaps/mmaps) |
| Build OOM-killed | Give Docker more RAM, or set `BUILD_JOBS` lower in `.env` |
| Client can't log in | Set `LOGIN_REST_EXTERNAL_ADDRESS` to a reachable IP; open 8081/1119/8085 |

---

# 🐳 Развёртывание через Docker Compose (RU)

Запускает весь стек — MySQL, worldserver, bnetserver — в контейнерах. Ядро
собирается из исходников TrinityCore внутри образа, наши community-фиксы
применяются автоматически, а два «близзардовских» входа ты подкладываешь сам.

> Это **контейнерный** путь. Нужна ручная установка? Смотри
> [ветку main](https://github.com/RomanKern89/wow-midnight-server) — оба способа
> ведут к одному серверу.

## ✅ Проверено с нуля

Прогнано end-to-end на **чистой Ubuntu 24.04 VM** (`docker compose down -v --rmi
local` → `up -d --build` от нуля):

- **Образ собирается из исходников** — компилирует TrinityCore master (~30 мин), 0 ошибок, образ ~7 ГБ.
- **MySQL** → `healthy`; **db-init** → создаёт `auth`/`characters`/`world`/`hotfixes`, импортирует базовые схемы (auth 39 + characters 132 таблицы), применяет наши фиксы, выходит с `0` — чистый лог, без варнингов.
- **bnetserver** → генерит self-signed dev-сертификат и работает; порты **1119 + 8081 + 3306** слушают.
- **worldserver** → с реальными данными (world + hotfixes дампы + данные клиента, на VM с 20 ГБ) **полностью грузится**: *«World initialized in 4 minutes 12 seconds»*, все четыре порта (1119/8081/8085/3306) живы, 0 рестартов. Без данных доходит до проверки и ждёт — это требование данных Blizzard, **а не баг**.

## Что предоставляешь ты (собственность Blizzard — в репо нет)

| Вход | Куда | Примечание |
|------|------|-----------|
| Данные из клиента (dbc/maps/vmaps/mmaps) | `docker/data/` | Из своего ретейл-клиента (билд 68275) |
| Дамп `world.sql` @68275 | `docker/import/` | квесты/спавны/лут (+ наши фиксы сверху) |
| Дамп `hotfixes.sql` @68275 | `docker/import/` | **обязателен** — без него worldserver крашлупит |

Всё остальное — создание схем, импорт баз auth/characters, **наши фиксы**,
рендер конфигов, самоподписанный логин-сертификат — автоматически. Как снять
дампы — в [import/README.md](import/README.md).

## Требования

- Docker Engine + Docker Compose v2
- **Сборка:** 24 ГБ+ RAM и время (ядро компилируется из исходников, ~30 мин)
- **Запуск (полный worldserver):** **16–20 ГБ RAM** — полный ретейл-мир (≈734к
  спавнов + карты) при 8 ГБ падает от OOM. MySQL + bnetserver отдельно — ~2 ГБ.
- Свободные порты: `8085` (мир), `1119` (bnet), `8081` (REST-логин), `3306` (MySQL)

## Шаги

```bash
cd docker
cp .env.example .env
# правишь .env: пароли + REALM_ADDRESS / LOGIN_REST_EXTERNAL_ADDRESS
#               = IP, к которому будет подключаться клиент.

# 1) положи дамп мира в ./import/  и данные клиента в ./data/

# 2) сборка + запуск
docker compose up -d --build      # первая сборка долгая (компиляция TrinityCore)

# 3) смотри, как поднимается
docker compose logs -f db-init        # импорт БД + фиксы
docker compose logs -f worldserver    # первый старт строит кэши (~минуты)
```

Создай игровой аккаунт (у worldserver интерактивная консоль):

```bash
docker attach tc-worldserver          # выйти: Ctrl-P Ctrl-Q
# в консоли:
account create myname mypassword
account set gmlevel myname 3 -1
```

Затем направь клиент на хост — см.
[../docs/ru/CONNECT.md](../docs/ru/CONNECT.md).

### Два состояния запуска
- **Без твоих данных** (пустые `import/` + `data/`): MySQL, инициализация БД и
  **логин-сервер bnetserver поднимаются** — годится проверить стек. `worldserver`
  будет в рестарт-цикле (нет карт / пустая база мира).
- **С твоими данными** (мир-дамп в `import/`, данные клиента в `data/`):
  `worldserver` полностью грузится и реалм готов к заходу в игру.

## Частые проблемы

| Симптом | Решение |
|---------|---------|
| `db-init`: «No world DB dump found» | Положи `.sql` в `docker/import/` и `docker compose up db-init` |
| worldserver падает: нет maps | Смонтируй данные клиента в `docker/data/` (dbc/maps/vmaps/mmaps) |
| Сборку убил OOM | Дай Docker больше RAM или уменьши `BUILD_JOBS` в `.env` |
| Клиент не логинится | Задай `LOGIN_REST_EXTERNAL_ADDRESS` = доступный IP; открой 8081/1119/8085 |
