# 🐳 Docker Compose deployment

**EN** · [Русский ниже](#-развёртывание-через-docker-compose-ru)

Run the whole stack — MySQL, worldserver, bnetserver — in containers. The core
is built from TrinityCore source inside the image, our community fixes are
applied automatically, and you supply the two Blizzard-owned inputs yourself.

> This is the **containerized** path. Prefer a manual install? See the
> [main branch](https://github.com/RomanKern89/wow-midnight-server) — both ways
> lead to the same server.

## What you provide (Blizzard's property — not shipped)

| Input | Where | Notes |
|-------|-------|-------|
| Client-extracted data (dbc/maps/vmaps/mmaps) | `docker/data/` | From your own retail client (build 68275) |
| A world DB dump @68275 | `docker/import/*.sql` | Imported on first run |

Everything else — schema creation, base auth/character import, **our fixes**,
config rendering, a self-signed login cert — is automatic.

## Requirements

- Docker Engine + Docker Compose v2
- **24 GB+ RAM** and time for the first build (the core compiles from source)
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

## Что предоставляешь ты (собственность Blizzard — в репо нет)

| Вход | Куда | Примечание |
|------|------|-----------|
| Данные из клиента (dbc/maps/vmaps/mmaps) | `docker/data/` | Из своего ретейл-клиента (билд 68275) |
| Дамп базы мира @68275 | `docker/import/*.sql` | Импортируется при первом запуске |

Всё остальное — создание схем, импорт баз auth/characters, **наши фиксы**,
рендер конфигов, самоподписанный логин-сертификат — автоматически.

## Требования

- Docker Engine + Docker Compose v2
- **24 ГБ+ RAM** и время на первую сборку (ядро компилируется из исходников)
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

## Частые проблемы

| Симптом | Решение |
|---------|---------|
| `db-init`: «No world DB dump found» | Положи `.sql` в `docker/import/` и `docker compose up db-init` |
| worldserver падает: нет maps | Смонтируй данные клиента в `docker/data/` (dbc/maps/vmaps/mmaps) |
| Сборку убил OOM | Дай Docker больше RAM или уменьши `BUILD_JOBS` в `.env` |
| Клиент не логинится | Задай `LOGIN_REST_EXTERNAL_ADDRESS` = доступный IP; открой 8081/1119/8085 |
