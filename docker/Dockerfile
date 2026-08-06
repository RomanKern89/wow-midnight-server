# =====================================================================
# TrinityCore master (retail, build 12.0.7-compatible) — multi-stage build.
# Stage 1 compiles the core from source; stage 2 is a slim runtime image.
#
# Build is heavy: give Docker 24 GB+ RAM and expect a long first build. The
# companion-bot scripts are compiled in as part of this stage.
# Data-extraction tools are intentionally OFF (TOOLS=0) — you extract client
# data on your own WoW client machine, then mount it (see docker/README.md).
# =====================================================================

# ---------- build stage ----------
FROM ubuntu:24.04 AS build
ARG TC_BRANCH=master
# Pinned by default to the revision this repository's fixes and companion-bot
# scripts are known to compile and run against. TrinityCore master moves daily;
# an unpinned clone means the image you build tomorrow is not the image that was
# tested today, and the first sign of that is a compile error in someone else's
# checkout. Override with --build-arg TC_COMMIT= (empty) to take branch tip.
ARG TC_COMMIT=c9e3fd8df5cb
ARG BUILD_JOBS=
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake make gcc g++ clang \
      libboost-all-dev libssl-dev libmysqlclient-dev \
      libreadline-dev zlib1g-dev libbz2-dev \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN git clone -b ${TC_BRANCH} https://github.com/TrinityCore/TrinityCore.git /src  && if [ -n "${TC_COMMIT}" ]; then git -C /src checkout --detach "${TC_COMMIT}"; fi  && git -C /src log -1 --format='building TrinityCore %h %cd' 

# Companion bots (see bots/README.md) — dropped into the standard custom-script
# directory BEFORE cmake runs, so they are compiled into the same binary. They
# add no core patches; TrinityCore's own custom_script_loader picks them up.
COPY bots/src /src/src/server/scripts/Custom/Bots/
# ...and the loader that actually registers them. TrinityCore ships an EMPTY
# AddCustomScripts(); leave it in place and the scripts compile, link, and are
# never called — a clean build of a server where .pbot does not exist.
COPY bots/custom_script_loader.cpp /src/src/server/scripts/Custom/custom_script_loader.cpp

WORKDIR /src/build
RUN cmake .. \
      -DCMAKE_INSTALL_PREFIX=/opt/tc \
      -DCMAKE_BUILD_TYPE=Release \
      -DTOOLS=0 -DSCRIPTS=static -DWITH_WARNINGS=0 \
 && make -j"${BUILD_JOBS:-$(nproc)}" \
 && make install
# keep the base SQL that ships with the source (auth/characters/hotfixes/world updates)
RUN cp -r /src/sql /opt/tc/sql-base

# ---------- runtime stage ----------
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# libboost-all-dev is a reliable superset (correct sonames regardless of point
# release) — favours "just works" over a minimal image.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libboost-all-dev \
      libssl3 libmysqlclient21 libreadline8 zlib1g libbz2-1.0 \
      default-mysql-client gettext-base openssl ca-certificates tini \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/tc /opt/tc
COPY docker/scripts /scripts
RUN chmod +x /scripts/*.sh

ENV PATH="/opt/tc/bin:${PATH}"
WORKDIR /opt/tc/bin
ENTRYPOINT ["/usr/bin/tini", "--"]
