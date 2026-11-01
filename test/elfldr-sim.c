// Starts a static ELF the way the PS5's ELF loader (elfldr, port 9021) does:
// loads the PT_LOAD segments at their addresses and calls the entry like a
// C function, with a return address on the stack and rdi = the loader's
// arguments. CI runs the Linux build of the program through it to check
// node/entry_*_amd64.s.
//
//   cc -static -Wl,-Ttext-segment=0x40000000 -o elfldr-sim test/elfldr-sim.c
//   ./elfldr-sim program.elf
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
  unsigned char *f = malloc(st.st_size);
  if (fd < 0 || read(fd, f, st.st_size) != st.st_size) { perror(argv[1]); return 1; }
  Elf64_Ehdr *eh = (void *)f; Elf64_Phdr *ph = (void *)(f + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    unsigned long a = ph[i].p_vaddr & ~0xfffUL, end = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffUL;
    void *m = mmap((void *)a, end - a, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy((void *)ph[i].p_vaddr, f + ph[i].p_offset, ph[i].p_filesz);
  }
  long (*entry)(void *) = (void *)eh->e_entry;
  fprintf(stderr, "elfldr-sim: calling entry %#lx\n", (unsigned long)eh->e_entry);
  return (int)entry((void *)0x1234);
}
