#!/usr/bin/env bash
# PreToolUse guard: no commit on master/main, no stash, no merge / gh pr merge,
# no reading .env, no push unless rebased onto the integration branch.
set -uo pipefail

input=$(cat)
tool=$(printf '%s' "$input" | jq -r '.tool_name // ""')
cwd=$(printf '%s' "$input" | jq -r '.cwd // ""')
[ -z "$cwd" ] && cwd="$PWD"

deny() {
  jq -n --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
  exit 0
}

if [ "$tool" = "Read" ]; then
  fp=$(printf '%s' "$input" | jq -r '.tool_input.file_path // ""')
  case "$(basename "$fp")" in
    .env|.env.*|*.env) deny "Чтение .env запрещено. Проверяй наличие (test -f / ls), переменные: source ${fp%/*}/.env" ;;
  esac
  exit 0
fi

[ "$tool" = "Bash" ] || exit 0
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // ""')
[ -z "$cmd" ] && exit 0

sanitized=$(printf '%s' "$cmd" | perl -0777 -pe 's/<<-?\s*[\x27"]?(\w+)[\x27"]?.*?^\1\b//gms; s/\x27[^\x27]*\x27//g; s/"[^"]*"//g;' 2>/dev/null || printf '%s' "$cmd")
GITPRE='git([[:space:]]+(-C[[:space:]]+[^[:space:]]+|-[^[:space:]]+))*[[:space:]]+'

if printf '%s' "$sanitized" | grep -Eq '(^|[[:space:]/])(cat|head|tail|less|more|bat|nano|vi|vim|nl|xxd|od|strings|grep|egrep|fgrep)[[:space:]]' \
   && printf '%s' "$sanitized" | grep -Eq '\.env([._-][A-Za-z0-9]+)*([[:space:]"'\''`]|$)'; then
  deny "Чтение содержимого .env запрещено. Проверяй только наличие файла, переменные: source <path>/.env"
fi
if printf '%s' "$sanitized" | grep -Eq "${GITPRE}stash([[:space:]]|\$)"; then
  deny "git stash запрещён. Временный коммит: git add -A && git commit -m 'wip'; …; git reset --soft HEAD~1."
fi
if printf '%s' "$sanitized" | grep -Eq "${GITPRE}merge([[:space:]]|\$)"; then
  deny "git merge запрещён — только rebase."
fi
if printf '%s' "$sanitized" | grep -Eq 'gh[[:space:]]+pr[[:space:]]+merge'; then
  deny "Мердж PR делает только человек. Не выполняй gh pr merge."
fi

target_dir() {
  local d
  d=$(printf '%s' "$cmd" | grep -oE 'git[[:space:]]+-C[[:space:]]+[^[:space:]]+' | tail -1 | grep -oE '[^[:space:]]+$')
  if [ -n "$d" ]; then printf '%s' "$d"; return; fi
  d=$(printf '%s' "$cmd" | grep -oE '(^|[;&|[:space:]])cd[[:space:]]+[^;&|]+' | tail -1 | sed -E 's/.*cd[[:space:]]+//; s/[[:space:]]+$//')
  if [ -n "$d" ]; then printf '%s' "$d"; return; fi
  printf '%s' "$cwd"
}

if printf '%s' "$sanitized" | grep -Eq "${GITPRE}commit([[:space:]]|\$)"; then
  dir=$(target_dir)
  branch=$(git -C "$dir" branch --show-current 2>/dev/null || true)
  if [ "$branch" = "master" ] || [ "$branch" = "main" ]; then
    deny "Коммит в '$branch' запрещён. Создай рабочую ветку."
  fi
fi

if printf '%s' "$sanitized" | grep -Eq "${GITPRE}push([[:space:]]|\$)"; then
  dir=$(target_dir)
  cur=$(git -C "$dir" branch --show-current 2>/dev/null || true)
  base=master
  if [ "$cur" != "master" ] && [ "$cur" != "main" ]; then
    if git -C "$dir" rev-parse --verify -q "origin/$base" >/dev/null 2>&1 \
       && ! git -C "$dir" merge-base --is-ancestor "origin/$base" HEAD 2>/dev/null; then
      deny "Push заблокирован: origin/$base не предок HEAD. Сначала: git checkout $base && git remote update && git pull && git checkout - && git rebase $base"
    fi
  fi
fi
exit 0
