#!/usr/bin/env bash
# Turn a clean official 1.1.2 checkout into YOUR fork: the updater points at your repo, and it builds
# on your repo (official ships no build workflow). Run from the repo root AFTER:
#     git fetch upstream && git reset --hard upstream/main
# Then review, commit, and push --force.
set -e
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

ME=klark231   # <- your GitHub user; change here if you ever rename

# 1) Point the updater (and its tests) at YOUR repo. The rejection tests (someone/else, evil.example)
#    are not github.com/gionko, so they stay as they are.
for f in AzaharLink/ConfigApp/source/update_logic.hpp \
         AzaharLink/Tests/settings_test/update_test.cpp \
         AzaharLink/Tests/update_http_test.cpp; do
  [ -f "$f" ] && sed -i \
    -e "s#github.com/gionko/3DS-Link#github.com/$ME/3DS-Link#g" \
    -e "s#githubusercontent.com/gionko/3DS-Link#githubusercontent.com/$ME/3DS-Link#g" "$f"
done
echo "updater -> github.com/$ME/3DS-Link"

# 2) A build workflow so it builds on your repo, and a stuck Docker pull FAILS in 30 min instead of
#    hanging for 6 hours. Optional Docker Hub login (set DOCKERHUB_USERNAME/DOCKERHUB_TOKEN repo secrets)
#    lifts the anonymous pull limit that was the likely cause of the hang.
mkdir -p .github/workflows
cat > .github/workflows/build.yml <<'YML'
name: build
on:
  push:
  workflow_dispatch:

permissions:
  contents: write

jobs:
  build:
    runs-on: ubuntu-latest
    timeout-minutes: 30          # a stalled Docker pull fails here, not after 6 hours
    steps:
      - uses: actions/checkout@v4

      - run: bash AzaharLink/Tests/policy_check.sh

      # Optional: authenticated Docker Hub pulls (higher rate limit). Set repo secrets
      # DOCKERHUB_USERNAME and DOCKERHUB_TOKEN to enable; without them it pulls anonymously.
      - name: Docker Hub login (optional)
        env:
          DH_USER: ${{ secrets.DOCKERHUB_USERNAME }}
          DH_TOKEN: ${{ secrets.DOCKERHUB_TOKEN }}
        run: |
          if [ -n "$DH_TOKEN" ]; then
            echo "$DH_TOKEN" | docker login -u "$DH_USER" --password-stdin
          else
            echo "no Docker Hub secrets set: pulling anonymously"
          fi

      - uses: actions/cache@v4
        id: ctr_tools
        with:
          path: |
            tools_ctr/makerom
            tools_ctr/ctrtool
          key: ctr-tools-${{ hashFiles('tools_ctr/build_tools.sh') }}

      - if: steps.ctr_tools.outputs.cache-hit != 'true'
        run: bash tools_ctr/build_tools.sh

      - run: bash DlpLink/tools/build_all.sh --dist

      - uses: actions/upload-artifact@v4
        with:
          name: 3DSLink
          path: |
            AzaharLink/AzaharLink.3gx
            AzaharLink/ConfigApp/3DSLink.3dsx
            DlpLink/out/3dslink.cia

      - uses: softprops/action-gh-release@v2
        if: github.ref == 'refs/heads/main'
        with:
          tag_name: build-${{ github.run_number }}
          prerelease: true
          make_latest: false
          files: |
            AzaharLink/AzaharLink.3gx
            AzaharLink/ConfigApp/3DSLink.3dsx
            DlpLink/out/3dslink.cia
YML
echo "wrote .github/workflows/build.yml (30-min timeout + optional Docker login)"
echo
echo "Now:  git add -A && git commit -m 'Reset to official 1.1.2; updater -> $ME; add build CI' && git push --force"
