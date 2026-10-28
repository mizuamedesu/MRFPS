FROM scratch AS context-audit
COPY . /context/

FROM debian:bookworm-slim AS runtime-base
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libatomic1 libstdc++6 libgcc-s1 libcurl4 libssl3 python3-minimal \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 mrfps \
    && useradd --uid 10001 --gid mrfps --create-home mrfps
FROM runtime-base AS server
WORKDIR /opt/mrfps
COPY --chown=10001:10001 Artifacts/Packages/LinuxServer/ /opt/mrfps/
COPY Deploy/Linux/entrypoint.sh /usr/local/bin/mrfps-entrypoint
COPY Deploy/Linux/healthcheck.py /usr/local/bin/mrfps-healthcheck.py
COPY Deploy/Linux/auth_config.py /usr/local/bin/auth_config.py
RUN chmod 755 /usr/local/bin/mrfps-entrypoint \
    && chmod -R a+rX /opt/mrfps \
    && find /opt/mrfps/MRFPS/Binaries/Linux -maxdepth 1 -name 'MRFPSServer*' -type f -exec chmod 755 {} + \
    && mkdir -p /opt/mrfps/MRFPS/Saved \
    && chown -R 10001:10001 /opt/mrfps/MRFPS/Saved
USER 10001:10001
ENV HOME=/home/mrfps MRFPS_PORT=7777
EXPOSE 7777/udp
HEALTHCHECK --interval=15s --timeout=5s --start-period=45s --retries=3 \
    CMD ["/usr/bin/python3", "/usr/local/bin/mrfps-healthcheck.py"]
ENTRYPOINT ["/usr/local/bin/mrfps-entrypoint"]
CMD ["/Game/Maps/MRFPSArena"]
