# MiniZinc Challenge 2026 用の sabori_csp 応募イメージ
#
# Build (リポジトリルートをコンテキストにすること):
#   docker build -f benchmarks/minizinc_challenge/Dockerfile -t sabori_csp:mznc2026 .
#
# Run example:
#   docker run --rm -v $(pwd)/aircraft.fzn:/p/aircraft.fzn sabori_csp:mznc2026 \
#     minizinc --time-limit 60000 /p/aircraft.fzn
#
# Push:
#   docker tag  sabori_csp:mznc2026 <docker_user>/sabori_csp:mznc2026
#   docker push <docker_user>/sabori_csp:mznc2026

# ---- Stage 1: build ----
FROM minizinc/mznc2026:latest AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
        bison \
        build-essential \
        cmake \
        flex \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# テストや Python バインディングはイメージに不要
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_FZN=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_PYTHON_BINDINGS=OFF && \
    cmake --build build --config Release -j"$(nproc)"

# ---- Stage 2: runtime ----
FROM minizinc/mznc2026:latest

# /sabori_csp 配下に必要なものだけ配置:
#   /sabori_csp/bin/fzn_sabori
#   /sabori_csp/share/minizinc/sabori_csp/...   (mznlib, redefinitions.mzn 等)
#   /sabori_csp/share/minizinc/solvers/sabori_csp.msc
COPY --from=builder /src/build/src/fzn/fzn_sabori \
                    /sabori_csp/bin/fzn_sabori
# build/share 側を使う (CMake が *.mzn のみ抽出してコピー済み。エディタ
# バックアップ等が混入しない)
COPY --from=builder /src/build/share/minizinc/sabori_csp \
                    /sabori_csp/share/minizinc/sabori_csp

# コンテナ内の絶対パスで .msc を生成 (build 時に CMake が出すものは
# ホスト側の絶対パスを含むので、ここで作り直す)
RUN mkdir -p /sabori_csp/share/minizinc/solvers && \
    printf '%s\n' \
        '{' \
        '  "name": "sabori_csp",' \
        '  "version": "1.0.0",' \
        '  "id": "io.github.tofjw.sabori_csp",' \
        '  "executable": "/sabori_csp/bin/fzn_sabori",' \
        '  "mznlib": "/sabori_csp/share/minizinc/sabori_csp",' \
        '  "tags": ["cp"],' \
        '  "stdFlags": ["-a"],' \
        '  "supportsMzn": false,' \
        '  "supportsFzn": true,' \
        '  "needsSolns2Out": true,' \
        '  "needsMznExecutable": false,' \
        '  "needsStdlibDir": false,' \
        '  "isGUIApplication": false' \
        '}' \
        > /sabori_csp/share/minizinc/solvers/sabori_csp.msc

# minizinc に対して --solver 指定なしでも sabori_csp を呼ばせる
RUN mkdir -p /root/.minizinc && \
    printf '%s\n' \
        '{' \
        '  "mzn_solver_path": ["/sabori_csp/share/minizinc/solvers"],' \
        '  "tagDefaults": [["", "io.github.tofjw.sabori_csp"]]' \
        '}' \
        > /root/.minizinc/Preferences.json

# (任意) sanity check: イメージビルド時に solver が認識されるか確認
RUN minizinc --solvers | grep -q sabori_csp
