#include <string.h>

#include <arpa/inet.h>

#include <rte_byteorder.h>

#include "../module.h"
#include "../utils/simd.h"

class VLanPush : public Module {
 public:
  virtual struct snobj *Init(struct snobj *arg);

  virtual void ProcessBatch(struct pkt_batch *batch);

  virtual struct snobj *GetDesc();

  struct snobj *RunCommand(const std::string &user_cmd, struct snobj *arg);

  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 1;

 private:
  struct snobj *CommandSetTci(struct snobj *arg);

  /* network order */
  uint32_t vlan_tag;
  uint32_t qinq_tag;
};

struct snobj *VLanPush::Init(struct snobj *arg) {
  struct snobj *t;

  if (!arg || snobj_type(arg) != TYPE_MAP)
    return snobj_err(EINVAL, "empty argument");

  if ((t = snobj_eval(arg, "tci")))
    return this->CommandSetTci(t);
  else
    return snobj_err(EINVAL, "'tci' must be specified");
}

/* the behavior is undefined if a packet is already double tagged */
void VLanPush::ProcessBatch(struct pkt_batch *batch) {
  int cnt = batch->cnt;

  uint32_t vlan_tag = this->vlan_tag;
  uint32_t qinq_tag = this->qinq_tag;

  for (int i = 0; i < cnt; i++) {
    struct snbuf *pkt = batch->pkts[i];
    char *new_head;
    uint16_t tpid;

    if ((new_head = static_cast<char *>(snb_prepend(pkt, 4))) != NULL) {
/* shift 12 bytes to the left by 4 bytes */
#if __SSE4_1__
      __m128i ethh;

      ethh = _mm_loadu_si128((__m128i *)(new_head + 4));
      tpid = _mm_extract_epi16(ethh, 6);

      ethh = _mm_insert_epi32(
          ethh, (tpid == rte_cpu_to_be_16(0x8100)) ? qinq_tag : vlan_tag, 3);

      _mm_storeu_si128((__m128i *)new_head, ethh);
#else
      tpid = *(uint16_t *)(new_head + 16);
      memmove(new_head, new_head + 4, 12);

      *(uint32_t *)(new_head + 12) =
          (tpid == rte_cpu_to_be_16(0x8100)) ? qinq_tag : vlan_tag;
#endif
    }
  }

  run_next_module(this, batch);
}

struct snobj *VLanPush::GetDesc() {
  uint32_t vlan_tag_cpu = ntohl(this->vlan_tag);

  return snobj_str_fmt("PCP=%u DEI=%u VID=%u", (vlan_tag_cpu >> 13) & 0x0007,
                       (vlan_tag_cpu >> 12) & 0x0001, vlan_tag_cpu & 0x0fff);
}

struct snobj *VLanPush::RunCommand(const std::string &user_cmd,
                                   struct snobj *arg) {
  if (user_cmd == "set_tci") {
    return this->CommandSetTci(arg);
  }
  assert(0);
}

struct snobj *VLanPush::CommandSetTci(struct snobj *arg) {
  uint16_t tci;

  if (!arg || snobj_type(arg) != TYPE_INT)
    return snobj_err(EINVAL, "argument must be an integer");

  tci = snobj_uint_get(arg);

  if (tci > 0xffff) return snobj_err(EINVAL, "TCI value must be 0-65535");

  this->vlan_tag = htonl((0x8100 << 16) | tci);
  this->qinq_tag = htonl((0x88a8 << 16) | tci);

  return NULL;
}

ModuleClassRegister<VLanPush> vlan_push("VLanPush", "vlan_push",
                                        "adds 802.1Q/802.11ad VLAN tag");
