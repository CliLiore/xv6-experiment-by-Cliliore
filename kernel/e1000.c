#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_bufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();
    if (!rx_bufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_bufs[i];
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(char *buf, int len)
{
  // 使用锁保护共享资源，防止多个CPU同时操作寄存器或环
  acquire(&e1000_lock);

  // 读取控制寄存器 E1000_TDT 获取下一个可用的 Tail 索引
  uint32 idx = regs[E1000_TDT];

  // 检查发送环是否溢出。检查 DD (Descriptor Done) 标志位
  // 如果为 0，说明网卡还没处理完这个位置之前的包
  if (!(tx_ring[idx].status & E1000_TXD_STAT_DD)) {
    release(&e1000_lock);
    return -1;
  }

  // 如果该位置之前有缓冲区，释放它（使用 kfree）
  if (tx_bufs[idx]) {
    kfree(tx_bufs[idx]);
  }

  // 填充描述
  // 根据错误提示，此时直接操作传入的 buf 和 len
  tx_bufs[idx] = buf;           // 保存指针用于以后释放
  tx_ring[idx].addr = (uint64)buf;
  tx_ring[idx].length = len;

  // 设置 cmd 标志位
  // EOP: End of Packet (报文结束); RS: Report Status (完成后设置 DD 位)
  tx_ring[idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // 更新 TDT 寄存器，取模防止越界
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0; // 成功
}

static void
e1000_recv(void)
{
  // 循环处理接收环中所有待处理的描述符
  while (1) {
    // 通过 RDT 获取下一个待处理的索引 idx
    uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    // 检查 DD 标志位判断是否有新包。如果不为 1 则退出
    if (!(rx_ring[idx].status & E1000_RXD_STAT_DD)) {
      break;
    }

    // 将收到的包传递给网络协议栈
    // net_rx 接收缓冲区地址和长度两个参数
    net_rx(rx_bufs[idx], rx_ring[idx].length);

    // 使用 kalloc 分配新缓冲区填补空位，使网卡能继续接收
    char *new_buf = kalloc();
    if (!new_buf) {
      panic("e1000_recv: kalloc failed");
    }
    rx_bufs[idx] = new_buf;
    rx_ring[idx].addr = (uint64)new_buf;
    rx_ring[idx].status = 0; // 清除 DD 位

    // 更新 RDT 寄存器
    regs[E1000_RDT] = idx;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
