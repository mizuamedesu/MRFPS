ARG MRFPS_BASE_IMAGE
FROM ${MRFPS_BASE_IMAGE}
USER 0:0
RUN if ! command -v setpriv >/dev/null 2>&1; then \
      apt-get update && apt-get install -y --no-install-recommends util-linux \
      && rm -rf /var/lib/apt/lists/*; fi
COPY flash-entrypoint.sh /usr/local/bin/mrfps-flash-entrypoint
COPY flash-healthcheck.sh /usr/local/bin/mrfps-flash-healthcheck
RUN sed -i 's/\r$//' /usr/local/bin/mrfps-flash-entrypoint \
    && sed -i 's/\r$//' /usr/local/bin/mrfps-flash-healthcheck \
    && chmod 755 /usr/local/bin/mrfps-flash-entrypoint /usr/local/bin/mrfps-flash-healthcheck
USER 10001:10001
HEALTHCHECK --interval=15s --timeout=5s --start-period=45s --retries=3 \
    CMD ["/usr/local/bin/mrfps-flash-healthcheck"]
ENTRYPOINT ["/usr/local/bin/mrfps-flash-entrypoint"]
CMD ["/Game/Maps/MRFPSArena"]
