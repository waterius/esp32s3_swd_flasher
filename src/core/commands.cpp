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
}

void cmd_info(const Host &h, gdb::LineFn emit, void *ectx) {
  if (!h.chip(h.ctx)) return emit(ectx, NO_CHIP);
  if (h.session_active(h.ctx)) return h.info(h.ctx, emit, ectx);
  if (!h.start_session(h.ctx, emit, ectx)) return;
  h.info(h.ctx, emit, ectx);
  h.end_session(h.ctx);  // короткий сеанс: плата перезапускается
}

void cmd_scan(const Host &h, gdb::LineFn emit, void *ectx) {
  const chip::Desc *d = h.chip(h.ctx);
  if (!d) return emit(ectx, NO_CHIP);
  if (!h.session_active(h.ctx) && !h.start_session(h.ctx, emit, ectx)) return;
  char line[64];
  snprintf(line, sizeof line, "1 %s", d->name);  // список целей, как у Black Magic Probe
  emit(ectx, line);
}

void cmd_baud(const Host &h, const char *arg, gdb::LineFn emit, void *ectx) {
  char line[64];
  if (*arg) {
    char *end = nullptr;
    unsigned long v = strtoul(arg, &end, 10);
    if (*end || v < BAUD_MIN || v > BAUD_MAX) {
      snprintf(line, sizeof line, "# скорость лога: от %lu до %lu", (unsigned long)BAUD_MIN,
               (unsigned long)BAUD_MAX);
      return emit(ectx, line);
    }
    h.set_baud(h.ctx, (uint32_t)v);
  }
  snprintf(line, sizeof line, "# лог: %lu", (unsigned long)h.baud(h.ctx));
  emit(ectx, line);
}

}  // namespace

void help(gdb::LineFn emit, void *ectx) {
  emit(ectx, "# команды:");
  emit(ectx, "#   help             эта подсказка");
  emit(ectx, "#   chip [имя]       выбрать чип; без имени — выбранный и поддержанные");
  emit(ectx, "#   info             подключиться и показать чип; перезапускает плату");
  emit(ectx, "#   reset            перезапустить плату");
  emit(ectx, "#   baud [скорость]  скорость лога платы, 1200–2000000");
  emit(ectx, "# в gdb — monitor <команда>, а также monitor swdp_scan");
}

void run(const Host &h, const char *line, gdb::LineFn emit, void *ectx) {
  char word[16];
  const char *arg;
  split(line, word, sizeof word, &arg);
  if (!word[0]) return;  // пустая строка
  if (strcmp(word, "help") == 0) return help(emit, ectx);
  if (strcmp(word, "chip") == 0) return cmd_chip(h, arg, emit, ectx);
  if (strcmp(word, "info") == 0) return cmd_info(h, emit, ectx);
  if (strcmp(word, "swdp_scan") == 0) return cmd_scan(h, emit, ectx);
  if (strcmp(word, "reset") == 0) {
    if (h.session_active(h.ctx)) return emit(ectx, BUSY);
    h.reset_board(h.ctx);
    return emit(ectx, "# сброс платы");
  }
  if (strcmp(word, "baud") == 0) return cmd_baud(h, arg, emit, ectx);
  help(emit, ectx);
}

}  // namespace commands
