/* ES40 emulator — headless shared-memory framebuffer publisher.
 *
 * Publishes the emulated S3 framebuffer into a POSIX-mmap file in the
 * streamhost "IFB1" wire format (the same one MAME's newport.cpp exports for
 * the IRIX tile), so a streamhost with SH_CAPTURE=shm reads frames straight
 * from the emulator with no X server and no window. Inert unless
 * ES40_SHM_PATH is set, so every ordinary SDL run is unaffected.
 *
 * WIRE FORMAT (little-endian, must match streamhost src/capture/shm.rs):
 *    off  type  field
 *      0  u32   magic 'IFB1' (0x31424649)
 *      4  u32   version (1)
 *      8  u32   width
 *     12  u32   height
 *     16  u32   stride (bytes per row)
 *     20  u32   bpp (32)
 *     24  u64   sequence (seqlock: odd while writing, even when stable)
 *     32  u32   dirty_x0  36 u32 dirty_y0  40 u32 dirty_x1  44 u32 dirty_y1
 *     48        pad to 64
 *     64        width*height XRGB8888 pixels (host-endian; B,G,R,X on x86)
 *
 * Pixels are the encoder's expected BGRA byte-for-byte — the same
 * bitmap_rgb32 layout the SDL path uploads — so there is no per-pixel
 * conversion anywhere.
 */
#ifndef ES40_GUI_SHMFB_H
#define ES40_GUI_SHMFB_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class CShmFramebuffer
{
public:
  static constexpr uint32_t kMagic = 0x31424649u; // 'IFB1'
  static constexpr size_t kHeader = 64;

  // Returns an active publisher when ES40_SHM_PATH is set, else nullptr.
  static CShmFramebuffer* create_if_enabled()
  {
    const char* path = getenv("ES40_SHM_PATH");
    if (!path || !*path)
      return nullptr;
    CShmFramebuffer* fb = new CShmFramebuffer();
    if (!fb->open_path(path))
    {
      delete fb;
      return nullptr;
    }
    return fb;
  }

  ~CShmFramebuffer()
  {
    if (m_map && m_map != MAP_FAILED)
      munmap(m_map, m_mapsize);
    if (m_fd >= 0)
      close(m_fd);
  }

  // Publish one frame. width*height XRGB8888 pixels, row stride in bytes
  // (defaults to width*4 — the S3 render bitmap is tightly packed).
  void publish(const uint32_t* pixels, uint32_t width, uint32_t height)
  {
    if (m_fd < 0 || width == 0 || height == 0)
      return;
    const uint32_t stride = width * 4u;
    if (width != m_width || height != m_height)
    {
      if (!resize(width, height, stride))
        return;
    }

    uint8_t* base = static_cast<uint8_t*>(m_map);
    volatile std::atomic<uint64_t>* seq =
        reinterpret_cast<volatile std::atomic<uint64_t>*>(base + 24);

    // seqlock: go odd (writing), copy pixels, go even (stable).
    const uint64_t s0 = m_seq + 1;
    seq->store(s0, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    memcpy(base + kHeader, pixels, static_cast<size_t>(stride) * height);

    // Whole-frame dirty: the S3 render path only calls us when the frame
    // actually changed, so every published frame is dirty end to end.
    put_u32(base + 32, 0);
    put_u32(base + 36, 0);
    put_u32(base + 40, width);
    put_u32(base + 44, height);

    std::atomic_thread_fence(std::memory_order_release);
    m_seq = s0 + 1;
    seq->store(m_seq, std::memory_order_release);
  }

private:
  int m_fd = -1;
  void* m_map = nullptr;
  size_t m_mapsize = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint64_t m_seq = 0;
  char m_path[4096] = {0};

  static void put_u32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

  bool open_path(const char* path)
  {
    snprintf(m_path, sizeof(m_path), "%s", path);
    m_fd = open(m_path, O_RDWR | O_CREAT, 0644);
    if (m_fd < 0)
    {
      printf("%%SHM-E-OPEN: cannot open %s for the framebuffer export.\n", m_path);
      return false;
    }
    // Header-only until the first frame sizes the mapping.
    return map_size(kHeader);
  }

  bool map_size(size_t bytes)
  {
    if (m_map && m_map != MAP_FAILED)
    {
      munmap(m_map, m_mapsize);
      m_map = nullptr;
    }
    if (ftruncate(m_fd, static_cast<off_t>(bytes)) != 0)
    {
      printf("%%SHM-E-TRUNC: ftruncate %s to %zu failed.\n", m_path, bytes);
      return false;
    }
    m_map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_map == MAP_FAILED)
    {
      printf("%%SHM-E-MMAP: mmap %s (%zu bytes) failed.\n", m_path, bytes);
      m_map = nullptr;
      return false;
    }
    m_mapsize = bytes;
    return true;
  }

  bool resize(uint32_t width, uint32_t height, uint32_t stride)
  {
    const size_t need = kHeader + static_cast<size_t>(stride) * height;
    // Park the sequence odd across the remap: a reader that maps the old,
    // smaller region mid-resize sees "writing" and retries after re-reading
    // geometry (streamhost re-maps on any width/height/stride change).
    if (!map_size(need))
      return false;

    uint8_t* base = static_cast<uint8_t*>(m_map);
    put_u32(base + 0, kMagic);
    put_u32(base + 4, 1);
    put_u32(base + 8, width);
    put_u32(base + 12, height);
    put_u32(base + 16, stride);
    put_u32(base + 20, 32);
    m_width = width;
    m_height = height;
    printf("%%SHM-I-GEOM: framebuffer export now %ux%u stride %u -> %s\n",
        width, height, stride, m_path);
    return true;
  }
};

#endif /* ES40_GUI_SHMFB_H */
