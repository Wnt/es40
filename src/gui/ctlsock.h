/* ES40 emulator — headless input control socket (mamectl/1).
 *
 * A unix-domain socket that speaks the streamhost "mamectl/1" wire protocol,
 * so the streamhost `mamesock` input backend drives a headless es40 exactly
 * as it drives the MAME IRIX tile — no X server, no window. Inert unless
 * ES40_CTL_SOCK is set.
 *
 * WIRE (line protocol; the client stamps each verb with a sequence number):
 *   server -> client, once per connection:
 *     HELLO mamectl/1 1 w2kalpha caps=natkbd,savest screen=WxH
 *   client -> server:
 *     <seq> MOVEA <x> <y>          absolute pointer target (screen pixels)
 *     <seq> MOVEP <dx> <dy>        relative pointer delta
 *     <seq> DOWN1|UP1 .. DOWN3|UP3 button edges (1 left, 2 right, 3 middle)
 *     <seq> KEY <0|1> <port> <field...>   key edge (0=up,1=down); es40 uses
 *                                          the field NAME, ignores the port
 *   server -> client, per verb:  <seq> OK   (or <seq> ERR <reason>)
 *
 * Pointer: MOVEA is absolute but the emulated PS/2 mouse is relative. W2K
 * draws a SOFTWARE cursor and parks the S3 hardware cursor off-screen, so
 * there is no HW-cursor position to close a loop against. Instead the guest
 * is configured for 1:1 pointer motion (no acceleration — MouseSpeed=0,
 * threshold 0, sensitivity 10; baked into the golden) and this path tracks
 * the believed position open-loop: the first target corner-homes to (0,0)
 * with a large negative slam, and every MOVEA then sends the exact remaining
 * delta. With 1:1 motion the believed position stays true, so absolute
 * targeting is pixel-accurate with no readback.
 *
 * Everything runs from the gui thread's handle_events() (~50 Hz); the socket
 * is non-blocking, one client at a time, reconnectable.
 */
#ifndef ES40_GUI_CTLSOCK_H
#define ES40_GUI_CTLSOCK_H

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../Keyboard.h"
#include "../VGA.h"
#include "gui.h"

// Streamhost mouse button bitmask, matching CKeyboard::mouse_motion's
// button_state (bit0 left, bit1 right, bit2 middle).
class CCtlSock
{
public:
  static CCtlSock* create_if_enabled(unsigned screen_w, unsigned screen_h)
  {
    const char* path = getenv("ES40_CTL_SOCK");
    if (!path || !*path)
      return nullptr;
    CCtlSock* s = new CCtlSock();
    if (!s->listen_on(path))
    {
      delete s;
      return nullptr;
    }
    s->m_w = screen_w;
    s->m_h = screen_h;
    return s;
  }

  ~CCtlSock()
  {
    if (m_client >= 0)
      close(m_client);
    if (m_listen >= 0)
      close(m_listen);
    if (!m_path.empty())
      unlink(m_path.c_str());
  }

  void set_screen(unsigned w, unsigned h)
  {
    m_w = w;
    m_h = h;
  }

  // Called from handle_events(): service the socket.
  void poll()
  {
    if (m_client < 0)
      accept_client();
    if (m_client >= 0)
      drain_client();
  }

private:
  int m_listen = -1;
  int m_client = -1;
  std::string m_path;
  std::string m_rx;
  unsigned m_w = 0, m_h = 0;

  // Open-loop pointer state: believed screen position and a corner-home flag.
  bool m_need_home = true;
  int m_bx = 0, m_by = 0;
  unsigned m_buttons = 0;

  bool listen_on(const char* path)
  {
    m_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listen < 0)
      return false;
    unlink(path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(m_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
      printf("%%CTL-E-BIND: cannot bind control socket %s\n", path);
      close(m_listen);
      m_listen = -1;
      return false;
    }
    if (listen(m_listen, 1) != 0)
    {
      close(m_listen);
      m_listen = -1;
      return false;
    }
    set_nonblock(m_listen);
    m_path = path;
    printf("%%CTL-I-LISTEN: mamectl input socket on %s\n", path);
    return true;
  }

  static void set_nonblock(int fd)
  {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }

  void accept_client()
  {
    int c = accept(m_listen, nullptr, nullptr);
    if (c < 0)
      return;
    set_nonblock(c);
    m_client = c;
    m_rx.clear();
    m_need_home = true;
    m_buttons = 0;
    char banner[128];
    snprintf(banner, sizeof(banner),
        "HELLO mamectl/1 1 w2kalpha caps=natkbd,savest screen=%ux%u\n", m_w,
        m_h);
    (void)!write(m_client, banner, strlen(banner));
    printf("%%CTL-I-CONN: mamectl client connected\n");
  }

  void close_client()
  {
    if (m_client >= 0)
      close(m_client);
    m_client = -1;
    m_rx.clear();
  }

  void drain_client()
  {
    char buf[1024];
    for (;;)
    {
      ssize_t n = read(m_client, buf, sizeof(buf));
      if (n > 0)
      {
        m_rx.append(buf, (size_t)n);
        size_t nl;
        while ((nl = m_rx.find('\n')) != std::string::npos)
        {
          std::string line = m_rx.substr(0, nl);
          m_rx.erase(0, nl + 1);
          handle_line(line);
        }
        continue;
      }
      if (n == 0)
      {
        close_client();
        return;
      }
      // n < 0: EAGAIN means done for now; anything else drops the client.
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        close_client();
      return;
    }
  }

  void ack(long seq, bool ok, const char* reason = "")
  {
    char out[96];
    int len = ok ? snprintf(out, sizeof(out), "%ld OK\n", seq)
                 : snprintf(out, sizeof(out), "%ld ERR %s\n", seq, reason);
    (void)!write(m_client, out, (size_t)len);
  }

  void handle_line(const std::string& line)
  {
    // "<seq> VERB args..."
    const char* s = line.c_str();
    char* end = nullptr;
    long seq = strtol(s, &end, 10);
    if (end == s)
      return; // no seq; ignore
    while (*end == ' ')
      end++;
    const char* verb = end;

    if (!strncmp(verb, "MOVEA ", 6))
    {
      int x = 0, y = 0;
      if (sscanf(verb + 6, "%d %d", &x, &y) == 2)
      {
        move_abs(clampi(x, 0, (int)m_w - 1), clampi(y, 0, (int)m_h - 1));
        ack(seq, true);
      }
      else
        ack(seq, false, "badargs");
      return;
    }
    if (!strncmp(verb, "MOVEP ", 6))
    {
      int dx = 0, dy = 0;
      if (sscanf(verb + 6, "%d %d", &dx, &dy) == 2)
      {
        inject_mouse(dx, dy);
        ack(seq, true);
      }
      else
        ack(seq, false, "badargs");
      return;
    }
    if (!strncmp(verb, "DOWN", 4) || !strncmp(verb, "UP", 2))
    {
      const bool down = verb[0] == 'D';
      const char btn = verb[down ? 4 : 2];
      unsigned bit = btn == '1' ? 1u : btn == '2' ? 2u : btn == '3' ? 4u : 0u;
      if (!bit)
      {
        ack(seq, false, "badbtn");
        return;
      }
      if (down)
        m_buttons |= bit;
      else
        m_buttons &= ~bit;
      inject_mouse(0, 0); // restate buttons with no motion
      ack(seq, true);
      return;
    }
    if (!strncmp(verb, "KEY ", 4))
    {
      int downi = 0;
      char port[16] = {0};
      char field[48] = {0};
      // "KEY <0|1> <port> <field with spaces>"
      if (sscanf(verb + 4, "%d %15s %47[^\n]", &downi, port, field) >= 3)
      {
        u32 code = field_to_bxkey(field);
        if (code == BX_KEYMAP_UNKNOWN)
          ack(seq, false, "unmappedkey");
        else
        {
          theKeyboard->gen_scancode(code | (downi ? BX_KEY_PRESSED
                                                  : BX_KEY_RELEASED));
          ack(seq, true);
        }
      }
      else
        ack(seq, false, "badargs");
      return;
    }
    ack(seq, false, "unknownverb");
  }

  static int clampi(int v, int lo, int hi)
  {
    return v < lo ? lo : v > hi ? hi : v;
  }

  // Inject a PS/2 delta. mouse_motion Y is positive-up while screen Y is
  // positive-down, so screen dy is negated (same convention as the SDL path).
  void inject_mouse(int screen_dx, int screen_dy)
  {
    if (theKeyboard)
      theKeyboard->mouse_motion(screen_dx, -screen_dy, 0, m_buttons);
  }

  // Absolute move, open-loop against the believed position. With the guest at
  // 1:1 motion the delta lands exactly, so the believed position stays true.
  void move_abs(int tx, int ty)
  {
    if (m_need_home)
    {
      // Slam far past the top-left so the guest clamps the cursor to (0,0),
      // establishing a known origin, then move to the target from there.
      inject_mouse(-(int)m_w - 256, -(int)m_h - 256);
      m_bx = 0;
      m_by = 0;
      m_need_home = false;
    }
    inject_mouse(tx - m_bx, ty - m_by);
    m_bx = tx;
    m_by = ty;
  }

  // Map a streamhost KEY field name to a BX_KEY code. Names come from
  // streamhost src/mame_input.rs KEY_MATRIX (third column).
  static u32 field_to_bxkey(const char* f)
  {
    struct Ent
    {
      const char* name;
      u32 code;
    };
    static const Ent kMap[] = {
        {"Esc", BX_KEY_ESC}, {"1", BX_KEY_1}, {"2", BX_KEY_2}, {"3", BX_KEY_3},
        {"4", BX_KEY_4}, {"5", BX_KEY_5}, {"6", BX_KEY_6}, {"7", BX_KEY_7},
        {"8", BX_KEY_8}, {"9", BX_KEY_9}, {"0", BX_KEY_0}, {"-", BX_KEY_MINUS},
        {"=", BX_KEY_EQUALS}, {"Backspace", BX_KEY_BACKSPACE},
        {"Tab", BX_KEY_TAB}, {"Q", BX_KEY_Q}, {"W", BX_KEY_W}, {"E", BX_KEY_E},
        {"R", BX_KEY_R}, {"T", BX_KEY_T}, {"Y", BX_KEY_Y}, {"U", BX_KEY_U},
        {"I", BX_KEY_I}, {"O", BX_KEY_O}, {"P", BX_KEY_P},
        {"[", BX_KEY_LEFT_BRACKET}, {"]", BX_KEY_RIGHT_BRACKET},
        {"Enter", BX_KEY_ENTER}, {"Left Ctrl", BX_KEY_CTRL_L}, {"A", BX_KEY_A},
        {"S", BX_KEY_S}, {"D", BX_KEY_D}, {"F", BX_KEY_F}, {"G", BX_KEY_G},
        {"H", BX_KEY_H}, {"J", BX_KEY_J}, {"K", BX_KEY_K}, {"L", BX_KEY_L},
        {";", BX_KEY_SEMICOLON}, {"'", BX_KEY_SINGLE_QUOTE},
        {"`", BX_KEY_GRAVE}, {"Left Shift", BX_KEY_SHIFT_L},
        {"\\", BX_KEY_BACKSLASH}, {"Z", BX_KEY_Z}, {"X", BX_KEY_X},
        {"C", BX_KEY_C}, {"V", BX_KEY_V}, {"B", BX_KEY_B}, {"N", BX_KEY_N},
        {"M", BX_KEY_M}, {",", BX_KEY_COMMA}, {".", BX_KEY_PERIOD},
        {"/", BX_KEY_SLASH}, {"Right Shift", BX_KEY_SHIFT_R},
        {"Keypad *", BX_KEY_KP_MULTIPLY}, {"Left Alt", BX_KEY_ALT_L},
        {"Space", BX_KEY_SPACE}, {"Caps Lock", BX_KEY_CAPS_LOCK},
        {"F1", BX_KEY_F1}, {"F2", BX_KEY_F2}, {"F3", BX_KEY_F3},
        {"F4", BX_KEY_F4}, {"F5", BX_KEY_F5}, {"F6", BX_KEY_F6},
        {"F7", BX_KEY_F7}, {"F8", BX_KEY_F8}, {"F9", BX_KEY_F9},
        {"F10", BX_KEY_F10}, {"Num Lock", BX_KEY_NUM_LOCK},
        {"Scroll Lock", BX_KEY_SCRL_LOCK}, {"Keypad 7", BX_KEY_KP_HOME},
        {"Keypad 8", BX_KEY_KP_UP}, {"Keypad 9", BX_KEY_KP_PAGE_UP},
        {"Keypad -", BX_KEY_KP_SUBTRACT}, {"Keypad 4", BX_KEY_KP_LEFT},
        {"Keypad 5", BX_KEY_KP_5}, {"Keypad 6", BX_KEY_KP_RIGHT},
        {"Keypad +", BX_KEY_KP_ADD}, {"Keypad 1", BX_KEY_KP_END},
        {"Keypad 2", BX_KEY_KP_DOWN}, {"Keypad 3", BX_KEY_KP_PAGE_DOWN},
        {"Keypad 0", BX_KEY_KP_INSERT}, {"Keypad .", BX_KEY_KP_DELETE},
        {"F11", BX_KEY_F11}, {"F12", BX_KEY_F12},
        {"INT1 56", BX_KEY_LEFT_BACKSLASH}, {"Keypad Enter", BX_KEY_KP_ENTER},
        {"Right Ctrl", BX_KEY_CTRL_R}, {"Keypad /", BX_KEY_KP_DIVIDE},
        {"Print Screen", BX_KEY_PRINT}, {"Right Alt", BX_KEY_ALT_R},
        {"Home", BX_KEY_HOME}, {"Cursor Up", BX_KEY_UP},
        {"Page Up", BX_KEY_PAGE_UP}, {"Cursor Left", BX_KEY_LEFT},
        {"Cursor Right", BX_KEY_RIGHT}, {"End", BX_KEY_END},
        {"Cursor Down", BX_KEY_DOWN}, {"Page Down", BX_KEY_PAGE_DOWN},
        {"Insert", BX_KEY_INSERT}, {"Delete", BX_KEY_DELETE},
        {"Left Win", BX_KEY_WIN_L}, {"Right Win", BX_KEY_WIN_R},
        {"Menu", BX_KEY_MENU},
    };
    for (const Ent& e : kMap)
      if (!strcmp(e.name, f))
        return e.code;
    return BX_KEYMAP_UNKNOWN;
  }
};

#endif /* ES40_GUI_CTLSOCK_H */
