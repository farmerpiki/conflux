FROM opensuse/tumbleweed:latest

RUN zypper --non-interactive refresh && \
    zypper --non-interactive install --no-recommends \
        bash \
        ca-certificates \
        Catch2-devel \
        clang21 \
        clang21-devel \
        cmake \
        gcc16-c++ \
        git \
        libbrotli-devel \
        libdeflate-devel \
        libopenssl-devel \
        liburing-devel \
        libzstd-devel \
        llvm21-libc++-devel \
        llvm21-libc++abi-devel \
        ninja \
        pkgconf-pkg-config \
        postgresql-devel \
        python313 \
        python313-pip \
        libnghttp2-devel \
        xxhash-devel && \
    ln -sf /usr/bin/python3.13 /usr/local/bin/python3 && \
    python3 -m pip install --break-system-packages --no-cache-dir "cmake>=4.2,<4.3" ninja && \
    ln -sf /usr/bin/clang-21 /usr/local/bin/clang && \
    ln -sf /usr/bin/clang++-21 /usr/local/bin/clang++ && \
    ln -sf /usr/bin/clang-scan-deps-21 /usr/local/bin/clang-scan-deps && \
    ln -sf /usr/bin/gcc-16 /usr/local/bin/gcc-16 && \
    ln -sf /usr/bin/g++-16 /usr/local/bin/g++-16 && \
    zypper clean --all

ENV PATH="/usr/local/bin:${PATH}"
