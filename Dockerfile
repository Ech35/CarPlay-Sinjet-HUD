FROM python:3.12-slim-bookworm

LABEL org.opencontainers.image.source="https://github.com/Ech35/CarPlay-Sinjet-HUD" \
      org.opencontainers.image.description="Offline navigation regression tests and Sinjet FB0 package builder"

RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/carplay-hud
COPY requirements.txt LICENSE THIRD_PARTY_NOTICES.md ./
RUN python -m pip install --no-cache-dir -r requirements.txt
COPY src/ble-r18/ src/ble-r18/
COPY src/hud-icons-r3/ src/hud-icons-r3/
COPY src/hud-bafb0-r1/ src/hud-bafb0-r1/
COPY assets/hud-icons/ assets/hud-icons/
COPY tools/hud_pak_tool.py tools/hud_pak_tool.py
RUN cc -O2 -shared -fPIC -o src/ble-r18/adapter-test.dll \
    src/ble-r18/core_adapter.c src/ble-r18/core/vendor/rgd/rgd_tlv.c
CMD ["python", "-B", "src/ble-r18/test_adapter.py"]
