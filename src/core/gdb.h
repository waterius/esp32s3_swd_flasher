// Смысл пакетов gdb: программатор отвечает как Black Magic Probe (design §9, таблица пакетов).
// Работает через функции цели — на плате их даёт port/session, в тестах — поддельная цель.
// Кадры — core/rsp, режимы порта — core/console.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core/chip.h"
#include "core/image.h"

namespace gdb {

using LineFn = void (*)(void *ctx, const char *line);

// Что команда программатора сделала с чипом: для ответа на vKill удачное erase — как удачная запись
// (спека gdb-server, «Провал заливки виден PlatformIO»).
enum class Outcome : uint8_t { None, WriteOk, WriteFailed };

struct Target {
  void *ctx;
  const chip::Desc *(*chip)(void *ctx);  // выбранный чип или nullptr
  bool (*session_active)(void *ctx);
  // Подключиться к плате и подготовить чип (design §4). Строки о подключении — в emit, если
  // он не nullptr, иначе в вывод программатора.
  bool (*start_session)(void *ctx, LineFn emit, void *emit_ctx);
  void (*end_session)(void *ctx);  // завершить по design §5: плата запускается
  bool (*read_mem)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
  // Регистры ядра: 0–12 r0–r12, 13 sp, 14 lr, 15 pc, 16 xpsr.
  bool (*read_reg)(void *ctx, uint8_t index, uint32_t *value);
  bool (*write_reg)(void *ctx, uint8_t index, uint32_t value);
  bool (*flash)(void *ctx, const image::Image &img);  // записать образ по design §8
  // Команда программатора, та же, что строкой в текстовом режиме (design §11).
  Outcome (*command)(void *ctx, const char *line, LineFn emit, void *emit_ctx);
  // Строка программатора «# …» в вывод порта (он выйдет, когда порт освободится).
  void (*print)(void *ctx, const char *line);
};

using SendFn = void (*)(void *ctx, const uint8_t *data, size_t len);

class Server {
 public:
  Server(const Target &target, image::Image &img, SendFn send, void *send_ctx);

  // Новое подключение gdb: подтверждения снова нужны до QStartNoAckMode.
  void reset_connection();
  // Пакет с верной суммой. true — после ответа порт выходит из режима gdb (vKill, D).
  bool on_packet(const uint8_t *data, size_t len);
  // Пакет с неверной суммой.
  void on_bad_packet();
  // Порт вышел из режима gdb не пакетом (закрыт, человек, тишина 60 с): завершить сеанс.
  void on_leave();

 private:
  void send_raw(const char *s, size_t n);
  void reply(const char *s, size_t n);
  void reply(const char *s);
  void reply_xfer(const char *doc, size_t doc_len, const char *args, size_t args_len);
  void end_session();
  bool refuse_image();
  static void emit_console(void *self, const char *line);

  const Target &t_;
  image::Image &img_;
  SendFn send_;
  void *send_ctx_;
  bool noack_ = false;
  bool write_ok_ = false;      // в сеансе была удачная запись
  bool write_failed_ = false;  // в сеансе была неудачная запись
  bool image_open_ = false;    // между первым vFlashErase и vFlashDone
};

}  // namespace gdb
