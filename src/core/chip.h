// Описание чипа: всё, что от чипа зависит, кроме действий через SWD (design §6).
// Чистые данные и правила — без Arduino, собираются и на ПК.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace chip {

enum class Kind : uint8_t { Flash, Ram };

// Область памяти чипа: куда оснастка вправе писать (Flash) или что gdb может читать (Ram).
struct Region {
  const char *name;   // в строках шагов: «# otp: совпадает»
  const char *title;  // в сообщениях: «OTP не стёрлась»
  uint32_t start;
  uint32_t size;
  uint32_t unit;           // единица стирания, Б; 0 — в область не пишем
  bool read_before_write;  // читать единицу чипа до решения (USER OTP)
  bool check_erase;        // после стирания убедиться, что всё 0xFF (USER OTP)
  Kind kind;               // вид для карты памяти gdb
  bool main;               // основная flash: её целиком стирает команда erase (web-flash-page design §8)
};

enum class Decision : uint8_t { Write, Skip, Refuse };

struct Verdict {
  Decision decision;
  const char *reason;  // для Refuse — текст для человека
};

// Решить про единицу: target — итоговая единица (байты образа, остальное 0xFF),
// current — та же единица в чипе, если область требует чтения, иначе nullptr.
using DecideFn = Verdict (*)(const Region &region, uint32_t addr, const uint8_t *target,
                             const uint8_t *current, uint32_t len);

// Тайминги flash: по регистру частоты, заводскому слову нужной частоты и словам заводской
// области — значения регистров таймингов или отказ.
struct Timing {
  bool ok;
  const char *reason;
  uint32_t regs[9];
};
using TimingFn = Timing (*)(uint32_t clock_reg, uint32_t clock_ref, const uint32_t info[5]);

struct Desc {
  const char *name;
  const Region *regions;
  uint8_t region_count;
  uint32_t id_addr;   // регистр идентификатора чипа
  uint32_t id_value;  // ожидаемое значение
  uint32_t uid_addr;
  uint8_t uid_len;
  DecideFn decide;
  TimingFn timing;  // nullptr — чип загрузки таймингов не требует
  // время стирания и записи единицы по даташиту, мкс
  uint32_t erase_typ_us, erase_max_us, write_typ_us, write_max_us;
};

// Область записи (Flash с единицей стирания), целиком содержащая [addr, addr + len),
// или nullptr.
const Region *find_region(const Desc &desc, uint32_t addr, uint32_t len);

// Основная flash чипа (отметка main) или nullptr.
const Region *main_region(const Desc &desc);

}  // namespace chip
