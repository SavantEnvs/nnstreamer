# syntax=docker/dockerfile:1.7
# nnstreamer commit image: builds the GStreamer tensor plugins (sanitized, for the fuzz harnesses
# in mayhem/harnesses/) and a normal-flags build whose nnstreamer-parser and gst elements
# mayhem/test.sh exercises.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
      meson ninja-build pkg-config flex bison python3 \
      libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libjson-glib-dev \
      libflatbuffers-dev flatbuffers-compiler libprotobuf-dev protobuf-compiler \
      gstreamer1.0-plugins-base gstreamer1.0-tools libgtest-dev \
 && rm -rf /var/lib/apt/lists/*
USER mayhem

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
