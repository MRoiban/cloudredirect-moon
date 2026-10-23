# Linux-hosted x64 Windows toolchain. xwin downloads Microsoft's SDK/CRT;
# building this image accepts their license (https://go.microsoft.com/fwlink/?LinkId=2086102).
FROM docker.io/library/ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates curl cmake ninja-build clang-19 lld-19 llvm-19 \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/lib/llvm-19/bin/clang /usr/local/bin/clang-cl \
 && ln -s /usr/lib/llvm-19/bin/lld /usr/local/bin/lld-link \
 && for tool in llvm-lib llvm-rc llvm-mt llvm-objdump llvm-readobj llvm-ar; do \
      ln -s /usr/lib/llvm-19/bin/$tool /usr/local/bin/$tool; \
    done

RUN curl -fsSL --retry 3 https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz -o /tmp/xwin.tar.gz \
 && echo '9fd53950b064d067f42428a69453b927656cae68dbd7f8d3f86dcb81c80dd22d  /tmp/xwin.tar.gz' | sha256sum -c - \
 && tar xzf /tmp/xwin.tar.gz -C /tmp \
 && mv /tmp/xwin-*/xwin /usr/local/bin/xwin \
 && rm -rf /tmp/xwin*

RUN xwin --accept-license --arch x86_64 --sdk-version 10.0.26100 --crt-version 14.44.17.14 splat --output /xwin \
 && rm -rf /.xwin-cache \
 && cd /xwin/sdk/lib/um/x86_64 \
 && for name in Shlwapi Advapi32 Crypt32 Shell32 Ole32 Psapi; do \
      lower=$(echo "$name" | tr A-Z a-z); \
      ln -s "$lower.lib" "$name.lib"; \
    done

WORKDIR /src
