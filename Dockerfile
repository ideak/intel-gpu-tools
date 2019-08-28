# Version number to bump for force image rebuild: 0
FROM build-fedora

RUN dnf install -y findutils && \
        dnf clean all

COPY opt-igt /opt/igt
COPY .gitlab-ci/docker-help.sh /usr/local/bin/docker-help.sh

ENV PATH="/opt/igt/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/igt/lib:/opt/igt/lib64:${LD_LIBRARY_PATH}"
ENV IGT_TEST_ROOT="/opt/igt/libexec/igt-gpu-tools"

CMD docker-help.sh
