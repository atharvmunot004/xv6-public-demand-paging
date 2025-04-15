#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
print_va_mapping(pde_t *pgdir, uint va)
{
  if (!pgdir) {
    cprintf("Invalid page directory.\n");
    return;
  }

  uint pdx = PDX(va);      // Page Directory Index
  uint ptx = PTX(va);      // Page Table Index
  uint offset = va & 0xFFF; // Offset within the page

  pde_t pde = pgdir[pdx];
  cprintf("VA: 0x%x\n", va);
  cprintf("PDE[%d] = 0x%x ", pdx, pde);

  if (!(pde & PTE_P)) {
    cprintf("(not present)\n");
    return;
  }

  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pde));
  pte_t pte = pgtab[ptx];

  cprintf("\n  -> Page Table at: %p (phys 0x%x)\n", pgtab, PTE_ADDR(pde));
  cprintf("PTE[%d] = 0x%x ", ptx, pte);

  if (!(pte & PTE_P)) {
    cprintf("(not present)\n");
    return;
  }

  uint pa = PTE_ADDR(pte) | offset;
  cprintf("-> PA = 0x%x | Flags:", pa);

  if (pte & PTE_P) cprintf(" P");
  if (pte & PTE_W) cprintf(" W");
  if (pte & PTE_U) cprintf(" U");
  if (!(pte & PTE_W)) cprintf(" R"); // Read-only if W not set

  cprintf("\n");
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir_dp_trap(pde_t *pgdir, const void *va, int alloc)
{
  cprintf ("walkpgdir_dp called:\n");
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  cprintf ("va: %x\n", va);
  cprintf("pde: %p\n", pde);
  cprintf("PDE INDEX: %d\n\n", PDX(va));
  if(*pde & PTE_P){
    cprintf ("Page table already present\n");
    cprintf ("pde: %x\n", *pde);
    cprintf ("PDE_ADDR: %x\n", PTE_ADDR(*pde));
    cprintf ("P2V: %x\n", P2V(PTE_ADDR(*pde)));
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P| PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}


// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages_dp_trap(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  cprintf ("mappags_dp called:\n");
  cprintf ("pgdir: %x\n", pgdir);
  cprintf ("va: %x\n", va);
  cprintf ("siz: %d\n", size);
  cprintf ("pa: %x\tpa: %d\n", pa, pa);
  cprintf ("perm: %d\n\n", perm);
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    cprintf ("Mapping pages:\n");
    cprintf ("a: %x\n", a);
    cprintf ("last: %x\n", last);
    if((pte = walkpgdir_dp_trap(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

void
add_page(struct proc *p, uint va)
{
  uint pa;
  pte_t *pgdir = p->pgdir;
  uint pdx = PDX(va);      // Page Directory Index
  uint ptx = PTX(va);      // Page Table Index
  uint offset = va & 0xFFF; // Offset within the page
  struct inode * pgdir_file = p->pgdir_inode;

  // Allocate a new page
  if ((pa = kalloc()) == 0) {
    cprintf("add_page: out of memory\n");
    panic ("add_page: out of memory");
    return;
  }
  memset(pa, 0, PGSIZE); // Clear the page
  cprintf ("The free address we got is: 0x%x\n", pa);

  if (readi(pgdir_file, pa, (NPDENTRIES * pdx) + (PGSIZE * ptx)  , PGSIZE) != PGSIZE) {
    cprintf("add_page: readi failed\n");
    panic ("add_page: readi failed");
    return;
  }

  uint perm = PTE_P | PTE_W | PTE_U;
  // if (mappages_dp_trap(pgdir, va, PGSIZE, (uint)pa, perm) < 0) {
  //   kfree(pa);
  //   panic("map failed");
  // }

  pde_t * pgtab = &pgdir[PDX(va)];
  char * pgframe = &pgtab[PTX(va)];

  *pgframe = pa | perm | PTE_P;

  cprintf ("the flags of the page table are: ");
  if ((uint)*pgframe & PTE_P) cprintf("P");
  if ((uint)*pgframe & PTE_W) cprintf("W");
  if ((uint)*pgframe & PTE_U) cprintf("U");
  cprintf ("\n");

  cprintf("pte: %d\n", PTX(va));
  cprintf("PTE_ADDR: %x\n", PTE_ADDR(*pgframe));
  cprintf("P2V: %x\n", P2V(PTE_ADDR(*pgframe)));
  // lcr3(V2P(p->pgdir)); // Flush the TLB
  cprintf("Added page at VA: 0x%x -> PA: 0x%x\n", va, pa);
  return;
}

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:
    char * va = rcr2();
    struct proc * p = myproc();
    cprintf("Works, Page Fault raised\n");
    cprintf ("The address accessed is: 0x%x\n", va);
    print_va_mapping(p->pgdir, va);
    pte_t * pgdir = p->pgdir;
    struct inode * pgdir_file = p->pgdir_inode; 
    
    add_page (p, va);

    cprintf ("The page has been added to the physical memory\n");
    lapiceoi();
    break;
  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
