#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "x86.h"
#include "elf.h"
#include "fcntl.h"
#include "stat.h"


static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}


// Helper to print flags in a readable format
void print_pde_flags(uint flags) {
  cprintf(" [");
  if (flags & PTE_P)  cprintf("P ");
  if (flags & PTE_W)  cprintf("W ");
  if (flags & PTE_U)  cprintf("U ");
  if (flags & PTE_PS) cprintf("PS "); // Page Size (not used in xv6 but part of x86)
  cprintf("]");
}

// Prints the page directory for a given pgdir
void print_pgdir(pde_t *pgdir) {
  cprintf("Page Directory at %p\n", pgdir);
  for (int i = 0; i < NPDENTRIES; i++) {
    if (pgdir[i] & PTE_P) {  // Only print present entries
      uint pa = PTE_ADDR(pgdir[i]);
      cprintf("PDE %d: PA=0x%x", i, pa);
      print_pde_flags(pgdir[i]);
      cprintf("\n");
    }
  }
}

void
print_pagetable(pde_t *pgdir)
{
  if (!pgdir)
    return;

  // Get the first page directory entry (PDE)
  pde_t pde = pgdir[0];
  if (!(pde & PTE_P)) {
    cprintf("PDE[0] not present.\n");
    return;
  }

  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pde)); // Convert to virtual addr
  cprintf("PDE[0]: %p -> page table at %p (phys %p)\n", &pgdir[0], pgtab, (char*)PTE_ADDR(pde));

  for (int i = 0; i < NPTENTRIES; i++) {
    // if (pgtab[i] & PTE_P) {
      uint pa = PTE_ADDR(pgtab[i]);
      cprintf("  PTE[%d]: VA 0x%08x -> PA 0x%08x | perms:", i, i * PGSIZE, pa);

      if (pgtab[i] & PTE_P)
        cprintf ("P");
      if (pgtab[i] & PTE_W)
        cprintf(" W");
      if (pgtab[i] & PTE_U)
        cprintf(" U");
      if (!(pgtab[i] & (PTE_W | PTE_U)))
        cprintf(" R");

      cprintf("\n");
    // }
  }
}

void
invalidate_page_table(pde_t *pgdir, uint pdx)
{
  pde_t pde = pgdir[pdx];
  if (!(pde & PTE_P)) {
    // cprintf("<invalidate_page_table>PDE[%d] not present.\n", pdx);
    return;
  }

  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pde));
  for (int i = 0; i < NPTENTRIES; i++) {
    pte_t pte = pgtab[i];
    if (pte & PTE_P) {
      char *pa = P2V(PTE_ADDR(pte));  // Get physical frame
      kfree(pa);                      // Free physical memory
      pgtab[i] = 0;                   // Clear PTE entirely
    }
  }

  // Optionally free the page table itself
  kfree((char*)pgtab);
  pgdir[pdx] = 0;

  // Optionally invalidate TLB
  lcr3(V2P(pgdir));

  cprintf("<invalidate_page_table>Freed page frames in page table at PDE[%d].\n", pdx);
}


struct inode*
save_pgdir_to_file(char* name, pde_t *pgdir)
{
  struct inode *ip;
  char * fname;
  char *pa;
  safestrcpy(fname, name, sizeof(fname));
  // Create a file with the name provided

  cprintf ("save_pgdir_to_file called:\n");
  cprintf ("checkpoint00\n");
  begin_op();

  // Create the file
  // use the sysfile.c create function
  // struct inode *create(char *path, short type, short major, short minor);
  ip = create(fname, T_FILE, 0, 0);
  if (ip == 0) {
    end_op();
    cprintf("Failed to create pgdir file\n");
    return 0;
  }

  cprintf ("checkpoint01\n");

  // ilock(ip);
  // Lock the inode for writing
  int file_offset = 0;

  cprintf ("checkpoint02\n");

  for (int pdx = 0; pdx < 512; pdx++) {
    if (!(pgdir[pdx] & PTE_P))
      continue;

    pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(pgdir[pdx]));

    for (int ptx = 0; ptx < NPTENTRIES; ptx++) {
      if (!(pgtab[ptx] & PTE_P))
        continue;

      // Compute virtual address for logging (optional)
      uint va = (pdx << PDXSHIFT) | (ptx << PTXSHIFT);

      pa = (char*)P2V(PTE_ADDR(pgtab[ptx]));  // Convert to kernel virtual address

      // Write 4096 bytes (one full page frame)
      int written = writei(ip, pa, file_offset, PGSIZE);
      if (written != PGSIZE) {
        cprintf("<copy pgdir>writei failed for va 0x%x at offset %d\n", va, file_offset);
        break;
      }

      file_offset += PGSIZE;

      if (file_offset >= 512 * PGSIZE)
        goto done; // Limit number of pages to prevent overflow
    }
  }

done:
  // iunlock(ip);
  end_op();

  cprintf("Saved %d bytes of page frame data to file \"%s\"\n", file_offset, fname);
  return ip;
}


int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();
  
  cprintf ("\n\n\n---------------------------------------------------------\n");
  cprintf ("starting: %s\n", path);
  cprintf ("exec called:\n");
  cprintf ("Process: %s\n\n", curproc->name);

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  
  // cprintf ("After setupkvm() the page directory looks like:\n");
  // print_pgdir(pgdir);

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    cprintf ("allocuvm() called for the first time:\n");
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;

    // cprintf ("After allocuvm() the page directory looks like:\n");
    // print_pgdir(pgdir);
    // cprintf("The first page table is:\n");
    // print_pagetable(pgdir);

    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    cprintf ("loaduvm() called:\n");
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
    
    // cprintf ("After loaduvm() the page directory looks like:\n");
    // print_pgdir(pgdir);
    // cprintf ("The first page table is:\n");
    // print_pagetable(pgdir);
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  cprintf ("Calling allocuvm() for the second time:\n");
  cprintf ("Process: %s\n", curproc->name);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;

  
  cprintf ("clearing the page directory:\n");

  save_pgdir_to_file(curproc->name, pgdir);
  for (int i = 0; i < 512; i ++) {
    invalidate_page_table(pgdir, 0); // Invalidate the first page table
  }

  

  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}
