#include "core/commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chips/chips.h"

namespace commands {

namespace {

const char NO_CHIP[] = "# чип не выбран: chip <имя>";
const char BUSY[] = "# идёт сеанс";

void supported(gdb::LineFn emit, void *ectx) {
  char line[128];
  int n = snprintf(line, sizeof line, "# поддержаны: ");
  for (uint8_t i = 0; i < chips::count && n > 0 && (size_t)n < sizeof line; i++)
    n += snprintf(line + n, sizeof line - n, "%s%s", i ? ", " : "", chips::all[i]->name);
  emit(ectx, line);
}

// Записываемые области выбранного чипа — имена для людей и для страницы (jaffa-flasher-page §4).
void regions(const chip::Desc &d, gdb::LineFn emit, void *ectx) {
  char line[96];
  for (uint8_t i = 0; i < d.region_count; i++) {
    const chip::Region &r = d.regions[i];
    if (r.kind != chip::Kind::Flash || !r.unit) continue;
    snprintf(line, sizeof line, "# область 0x%08lX–0x%08lX: %s%s", (unsigned long)r.start,
             (unsigned long)(r.start + r.size - 1), r.title, r.main ? ", основная" : "");
    emit(ectx, line);
  }
}

// Слово команды и аргумент: «baud 9600» → «baud», «9600».
void split(const char *line, char *word, size_t wsize, const char **arg) {
  size_t i = 0;
  while (line[i] && line[i] != ' ' && i < wsize - 1) {
    word[i] = line[i];
    i++;
  }
  word[i] = 0;
  while (line[i] && line[i] != ' ') i++;  // слишком длинное слово — до пробела
  while (line[i] == ' ') i++;
  *arg = line + i;
}

void cmd_chip(const Host &h, const char *arg, gdb::LineFn emit, void *ectx) {
  char line[96];
  if (!*arg) {
    const chip::Desc *d = h.chip(h.ctx);
    if (d) {
      snprintf(line, sizeof line, "# чип: %s", d->name);
      emit(ectx, line);
      regions(*d, emit, ectx);
    } else {
      emit(ectx, NO_CHIP);
    }
    return supported(emit, ectx);
  }
  if (h.session_active(h.ctx)) return emit(ectx, BUSY);
  const chip::Desc *d = chips::find(arg);
  if (!d) {
    snprintf(line, sizeof line, "# чип не поддержан: %s", arg);
    emit(ectx, line);
    return supported(emit, ectx);
  }
  h.set_chip(h.ctx, d);
  snprintf(line, sizeof line, "# чип: %s", d->name);
  emit(ectx, line);
  regions(*d, emit, ectx);
}

void cmd_info(const Host &h, gdb::LineFn emit, void *ectx) {
  if (!h.chip(h.ctx)) return emit(ectx, NO_CHIP);
  if (h.session_active(h.ctx)) return h.info(h.ctx, emit, ectx);
  if (!h.start_session(h.ctx, emit, ectx)) return;
  h.info(h.ctx, emit, ectx);
  h.end_session(h.ctx);  // короткий сеанс: плата перезапускается
}

// В сеансе gdb — в нём; вне сеанса — свой короткий сеанс, плата после него перезапускается.
gdb::Outcome cmd_erase(const Host &h, gdb::LineFn emit, void *ectx) {
  if (!h.chip(h.ctx)) return emit(ectx, NO_CHIP), gdb::Outcome::None;
  bool own = !h.session_active(h.ctx);
  if (own && !h.start_session(h.ctx, emit, ectx)) return gdb::Outcome::WriteFailed;
  bool ok = h.erase(h.ctx, emit, ectx);
  if (own) h.end_session(h.ctx);
  return ok ? gdb::Outcome::WriteOk : gdb::Outcome::WriteFailed;
}

void cmd_scan(const Host &h, gdb::LineFn emit, void *ectx) {
  const chip::Desc *d = h.chip(h.ctx);
  if (!d) return emit(ectx, NO_CHIP);
  if (!h.session_active(h.ctx) && !h.start_session(h.ctx, emit, ectx)) return;
  char line[64];
  snprintf(line, sizeof line, "1 %s", d->name);  // список целей, как у Black Magic Probe
  emit(ectx, line);
}

// baud [скорость [формат]]: без аргументов — показать; неверное — подсказка, ничего не меняется.
void cmd_baud(const Host &h, const char *arg, gdb::LineFn emit, void *ectx) {
  char line[80];
  LogFormat f = h.log(h.ctx);
  if (*arg) {
    char *end = nullptr;
    unsigned long v = strtoul(arg, &end, 10);
    if ((*end && *end != ' ') || v < BAUD_MIN || v > BAUD_MAX) {
      snprintf(line, sizeof line, "# скорость лога: от %lu до %lu", (unsigned long)BAUD_MIN,
               (unsigned long)BAUD_MAX);
      return emit(ectx, line);
    }
    while (*end == ' ') end++;
    if (*end && !parse_format(end, &f)) {
      return emit(ectx, "# формат лога: биты 5–8, чётность N, E или O, стоп 1 или 2 — например 8N1");
    }
    f.baud = (uint32_t)v;
    h.set_log(h.ctx, f);
  }
  snprintf(line, sizeof line, "# лог: %lu %u%c%u", (unsigned long)f.baud, f.data_bits, f.parity,
           f.stop_bits);
  emit(ectx, line);
}

}  // namespace

bool parse_format(const char *t, LogFormat *out) {
  if (!t[0] || !t[1] || !t[2] || t[3]) return false;
  if (t[0] < '5' || t[0] > '8') return false;
  if (t[1] != 'N' && t[1] != 'E' && t[1] != 'O') return false;
  if (t[2] != '1' && t[2] != '2') return false;
  out->data_bits = (uint8_t)(t[0] - '0');
  out->parity = t[1];
  out->stop_bits = (uint8_t)(t[2] - '0');
  return true;
}

void help(gdb::LineFn emit, void *ectx) {
  emit(ectx, "# команды:");
  emit(ectx, "#   help             эта подсказка");
  emit(ectx, "#   chip [имя]       выбрать чип; без имени — выбранный и поддержанные");
  emit(ectx, "#   info             подключиться и показать чип; перезапускает плату");
  emit(ectx, "#   erase            стереть всю основную flash (OTP не трогает); перезапускает плату");
  emit(ectx, "#   reset            перезапустить плату");
  emit(ectx, "#   baud [скорость [формат]]  лог платы: 1200–2000000, формат 8N1, 8E1…");
  emit(ectx, "# в gdb — monitor <команда>, а также monitor swdp_scan");
}

gdb::Outcome run(const Host &h, const char *line, gdb::LineFn emit, void *ectx, Source source) {
  char word[16];
  const char *arg;
  split(line, word, sizeof word, &arg);
  if (!word[0]) return gdb::Outcome::None;  // пустая строка
  if (strcmp(word, "erase") == 0) return cmd_erase(h, emit, ectx);
  if (strcmp(word, "help") == 0) help(emit, ectx);
  else if (strcmp(word, "chip") == 0) cmd_chip(h, arg, emit, ectx);
  else if (strcmp(word, "info") == 0) cmd_info(h, emit, ectx);
  else if (strcmp(word, "swdp_scan") == 0) {
    if (source != Source::Gdb) emit(ectx, "# swdp_scan — только через monitor в gdb");
    else cmd_scan(h, emit, ectx);
  } else if (strcmp(word, "reset") == 0) {
    if (h.session_active(h.ctx)) {
      emit(ectx, BUSY);
    } else {
      h.reset_board(h.ctx);
      emit(ectx, "# сброс платы");
    }
  } else if (strcmp(word, "baud") == 0) {
    cmd_baud(h, arg, emit, ectx);
  } else {
    help(emit, ectx);
  }
  return gdb::Outcome::None;
}

}  // namespace commands
