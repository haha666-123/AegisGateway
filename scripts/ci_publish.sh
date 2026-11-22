#!/usr/bin/env bash
# Publishes the build/test logs of a CI job to a dedicated branch so the
# results can be inspected without authenticating against the Actions API.
#
#   scripts/ci_publish.sh ci-report-linux-gcc
#
# Requires GH_TOKEN with contents:write (the default GITHUB_TOKEN is enough when
# the workflow declares `permissions: contents: write`).
set -euo pipefail

branch="${1:?usage: ci_publish.sh <report-branch>}"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"
sha="${GITHUB_SHA:-unknown}"
job="${GITHUB_JOB:-local}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for file in build.log test.log ldd.log; do
    if [[ -f "$file" ]]; then
        cp "$file" "$work/$file"
    fi
done

if [[ -f build/Testing/Temporary/LastTest.log ]]; then
    cp build/Testing/Temporary/LastTest.log "$work/ctest-last.log"
fi

{
    echo "job:     $job"
    echo "commit:  $sha"
    echo "run:     ${GITHUB_RUN_ID:-local} attempt ${GITHUB_RUN_ATTEMPT:-1}"
    echo "time:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "runner:  ${RUNNER_OS:-unknown} ${RUNNER_ARCH:-}"
    echo
    uname -a || true
    echo
    (cmake --version | head -n 1) || true
    (g++ --version | head -n 1) || true
    (ninja --version) || true
    echo
    echo "--- artefacts ---"
    ls -l build/bin 2>/dev/null || true
} > "$work/env.txt"

if [[ ! -f "$work/test.log" ]]; then
    echo "no test log was produced" > "$work/test.log"
fi

cd "$work"
git init -q
git checkout -q -b "$branch"
git config user.email "ci@aegis.local"
git config user.name "aegis-ci"
git add -A
git commit -qm "ci report: ${job} @ ${sha}"
git push -q -f "https://x-access-token:${GH_TOKEN}@github.com/${repo}.git" "HEAD:${branch}"
echo "report pushed to branch ${branch}"
