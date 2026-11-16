# A ForeverTAS worker in a container (docs/REMOTE.md): a rented GPU, a server.
#
#   docker build -t forevertas-worker worker/
#   docker run --rm --gpus all -v /path/to/TmForever/Packs:/packs:ro forevertas-worker CODES
#
# (CODES: what it searches for, separated by commas: your room's code, groups' invites, public.)
# (AMD: --device /dev/kfd --device /dev/dri instead of --gpus all.) The game's
# Packs come from your copy, mounted at /packs. FOREVERTAS_VERSION picks the
# ForeverTAS version (default: the latest release).
FROM ubuntu:22.04
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 ca-certificates zstd file \
        libgl1 libegl1 libopengl0 libfontconfig1 libfreetype6 libxkbcommon0 libdbus-1-3 libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*
COPY forevertas_worker.py /opt/forevertas/forevertas_worker.py
ENV FOREVERTAS_VERSION=latest
VOLUME /packs
ENTRYPOINT ["/bin/sh", "-c", "exec python3 /opt/forevertas/forevertas_worker.py --packs /packs --version \"$FOREVERTAS_VERSION\" --dir /opt/forevertas/cache --code \"$0\""]
