/*
 * tracker is an hybrid trustworthy disassembler that tries to limit the number
 * of false positive paths discovered.
 *
 *  Written and maintained by Emmanuel Fleury <emmanuel.fleury@u-bordeaux.fr>
 *
 * Copyright 2019 University of Bordeaux, CNRS (UMR 5800), France.
 * All rights reserved.
 *
 * This software is released under a 3-clause BSD license (see COPYING file).
 */

#include "tracker.h"
#include <graphviz/cgraph.h>

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <err.h>
#include <getopt.h>
#include <libgen.h>
#include <string.h>

#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <capstone/capstone.h>

#include <trace.h>

/* Platform architecture arch_t type */
typedef enum
  {
   unknown_arch,
   x86_32_arch,
   x86_64_arch
} arch_t;

/* In amd64, maximum bytes for an opcode is 15 */
#define MAX_OPCODE_BYTES 16

/* Maximum length of a line in input */
#define MAX_LEN 1024

/* Global variables for this module */
static bool debug = false;      /* 'debug' option flag */
static bool verbose = false;    /* 'verbose' option flag */
static FILE *output = NULL;     /* output file (default: stdout) */
/* input file containing executable's name and argument */
static FILE *input = NULL;

static FILE *fp = NULL;

/* Get the architecture of the executable */
static arch_t
check_execfile (char *execfilename)
{
  struct stat exec_stats;
  if (stat (execfilename, &exec_stats) == -1)
    err (EXIT_FAILURE, "error: '%s'", execfilename);

  if (!S_ISREG (exec_stats.st_mode) || !(exec_stats.st_mode & S_IXUSR))
    errx (EXIT_FAILURE, "error: '%s' is not an executable file", execfilename);

  /* Check if given file is an executable and discover its architecture */
  FILE *execfile = fopen (execfilename, "r");
  if (!execfile)
    err (EXIT_FAILURE, "error: '%s'", execfilename);

  /* Open file */
  char buf[4] = { 0 };
  if (fread (&buf, 4, 1, execfile) != 1)
    errx (EXIT_FAILURE, "error: cannot read '%s'", execfilename);

  /* Check ELF magic number (first 4 bytes: 0x7f "ELF") */
  if (buf[0] != 0x7F || strncmp (&(buf[1]), "ELF", 3))
    errx (EXIT_FAILURE, "error: '%s' is not an ELF binary", execfilename);

  /* Extract executable architecture (byte at 0x12) */
  fseek (execfile, 0x12, SEEK_SET);
  if (fread (&buf, 1, 1, execfile) != 1)
    errx (EXIT_FAILURE, "error: cannot read '%s'", execfilename);

  arch_t exec_arch = unknown_arch;
  switch (buf[0])
    {
    case 0x03:
      exec_arch = x86_32_arch;
      break;

    case 0x3E:
      exec_arch = x86_64_arch;
      break;

    default:
      errx (EXIT_FAILURE, "error: '%s' unsupported architecture", execfilename);
    }

  /* Closing file after verifications */
  fclose (execfile);

  return exec_arch;
}

/* Get current instruction pointer address */
static uintptr_t
get_current_ip (struct user_regs_struct *regs)
{
#if defined(__x86_64__) /* amd64 architecture */
      return regs->rip;
#elif defined(__i386__) /* i386 architecture */
      return regs->eip;
#else
#error Cannot build, we only support: x86-64 and i386 architectures
#endif
}

static void
get_text_info (const char *execfilename, uint64_t *text_addr, uint64_t *text_size)
{
  FILE *execfile = fopen (execfilename, "r");
  unsigned char buf[8];
  fseek (execfile, 0x28, SEEK_SET);
  fread (&buf, 8, 1, execfile);
  uint64_t e_shoff = 0;
  for (int i = 7; i >= 0; i--) {
    e_shoff = e_shoff << 8;
    e_shoff += buf[i];
  }
  fseek (execfile, 0x3A, SEEK_SET);
  fread (&buf, 2, 1, execfile);
  uint64_t e_shentsize = 0;
  for (int i = 1; i >= 0; i--) {
    e_shentsize = e_shentsize << 8;
    e_shentsize += buf[i];
  }
  fseek (execfile, 0x3C, SEEK_SET);
  fread (&buf, 2, 1, execfile);
  uint64_t e_shnum = 0;
  for (int i = 1; i >= 0; i--) {
    e_shnum = e_shnum << 8;
    e_shnum += buf[i];
  }
  fseek (execfile, 0x3E, SEEK_SET);
  fread (&buf, 2, 1, execfile);
  uint64_t e_shstrndx = 0;
  for (int i = 1; i >= 0; i--) {
    e_shstrndx = e_shstrndx << 8;
    e_shstrndx += buf[i];
  }
  fseek (execfile, e_shoff + (e_shentsize * e_shstrndx) + 0x18, SEEK_SET);
  fread (&buf, 8, 1, execfile);
  uint64_t shstrtab = 0;
  for (int i = 7; i >= 0; i--) {
    shstrtab = shstrtab << 8;
    shstrtab += buf[i];
  }
  uint64_t index = 0;
  while (true) {
    uint64_t var = 0;
    fseek (execfile, e_shoff + (e_shentsize * index), SEEK_SET);
    fread (&buf, 4, 1, execfile);
    for (int i = 3; i >= 0; i--) {
      var = var << 8;
      var += buf[i];
    }
    fseek (execfile, shstrtab + var, SEEK_SET);
    fread (&buf, 5, 1, execfile);
    if (buf[0] == '.' && buf[1] == 't' && buf[2] == 'e' && buf[3] == 'x' && buf[4] == 't')
      break;
    index++;
  }
  fseek (execfile, e_shoff + (e_shentsize * index) + 0x18, SEEK_SET);
  fread (&buf, 8, 1, execfile);
  *text_addr = 0;
  for (int i = 7; i >= 0; i--) {
    *text_addr = *text_addr << 8;
    *text_addr += buf[i];
  }
  fseek (execfile, e_shoff + (e_shentsize * index) + 0x20, SEEK_SET);
  fread (&buf, 8, 1, execfile);
  *text_size = 0;
  for (int i = 7; i >= 0; i--) {
    *text_size = *text_size << 8;
    *text_size += buf[i];
  }
  fclose (execfile);
  return;
}

static char *
concat_str (char *dest, char *follow)
{
	if (!dest)
	{
		dest = calloc ((strlen (follow) + 1), sizeof (char));
		sprintf (dest, "%s", follow);
		return dest;
	}
	dest = realloc (dest,
                  (strlen (dest) + 1 + strlen (follow) + 1) * sizeof (char));
	sprintf (dest + strlen(dest), "\n%s", follow);
	return dest;
}

static Agraph_t *
graph_create_function (Agraph_t *g, cfg_t *entry, Agnode_t *n)
{
  Agnode_t *m = NULL;
  cfg_t *old = entry;
	cfg_t *new = NULL;
  char *str_bb = NULL;
  while (cfg_get_type (old) == BASIC || cfg_get_type (old) == CALL)
    {
      /* Not the begining of the function + more than 1 parent --> basic block */
      if (old != entry && cfg_get_nb_in (old) > 1)
        {
          m = agnode (g, str_bb, TRUE);
          free(str_bb);
          str_bb = NULL;
          if (n)
            if (!agedge (g, n, m, NULL, FALSE))
              agedge (g, n, m, NULL, TRUE);
          graph_create_function (g, old, m);
          return g;
        }
      if (cfg_get_type (old) == CALL)
        {
          /* Searching for a RET following the CALL */
          uint16_t i = 0;
          while (i < cfg_get_nb_out (old))
            {
              new = cfg_get_successor_i (old, i);
              if (instr_get_addr (cfg_get_instr (old)) + instr_get_size (cfg_get_instr (old))
        		      == instr_get_addr (cfg_get_instr (new)))
                break;
              new = NULL;
              i++;
            }
          str_bb = concat_str (str_bb, cfg_get_str(old));
          if (!new)
            {
              m = agnode (g, str_bb, TRUE);
              free(str_bb);
              str_bb = NULL;
              if (n)
								if (!agedge (g, n, m, NULL, FALSE))
                  agedge (g, n, m, NULL, TRUE);
              return g;
            }
          else
            {
              old = new;
              new = NULL;
            }
        }
      else
        {
          str_bb = concat_str (str_bb, cfg_get_str(old));
          if (cfg_get_nb_out (old) == 0)
            {
              m = agnode (g, str_bb, TRUE);
              free(str_bb);
              str_bb = NULL;
              if (!agedge (g, n, m, NULL, FALSE))
                agedge (g, n, m, NULL, TRUE);
              return g;
            }
          old = cfg_get_successor_i (old, 0);
          /* Ugly trick to avoid infite loops if a instruction is its own parent */
          if (old == entry)
            {
              m = agnode (g, str_bb, TRUE);
              free(str_bb);
              str_bb = NULL;
              if (!agedge (g, n, m, NULL, FALSE))
                agedge (g, n, m, NULL, TRUE);
              Agnode_t *tmp = agnode (g, cfg_get_str (old), TRUE);
              agedge (g, m, tmp, NULL, TRUE);
              agedge (g, tmp, tmp, NULL, TRUE);
              return g;
            }
        }
    }
  /* Enf of a basic block */
  str_bb = concat_str (str_bb, cfg_get_str(old));
  m = agnode (g, str_bb, TRUE);
  free(str_bb);
  str_bb = NULL;
  if (n)
    {
      if (!agedge (g, n, m, NULL, FALSE))
        {
          agedge (g, n, m, NULL, TRUE);
          if (cfg_get_type (old) == BRANCH || cfg_get_type (old) == JUMP)
            {
              for (uint16_t i = 0; i < cfg_get_nb_out (old); i++)
                graph_create_function (g, cfg_get_successor_i (old, i), m);
            }
        }
    }
  else
    {
      if (cfg_get_type (old) == BRANCH || cfg_get_type (old) == JUMP)
        {
          for (uint16_t i = 0; i < cfg_get_nb_out (old); i++)
            graph_create_function (g, cfg_get_successor_i (old, i), m);
        }
    }
  return g;
}

int
main (int argc, char *argv[], char *envp[])
{
  /* Getting program name */
  const char *program_name = basename (argv[0]);

  /* Initializing output to its default */
  output = stdout;

  /* Options parser settings */
  opterr = 0; /* Mute error message from getopt() */
  const char *opts = "dio:vVh";

  bool intel = false;

   const struct option long_opts[] = {
    {"debug",          no_argument, NULL, 'd'},
    {"intel",          no_argument, NULL, 'i'},
    {"output",   required_argument, NULL, 'o'},
    {"verbose",        no_argument, NULL, 'v'},
    {"version",        no_argument, NULL, 'V'},
    {"help",           no_argument, NULL, 'h'},
    {NULL,                       0, NULL,   0}
  };

   const char *usage_msg =
     "Usage: %1$s [-o FILE|-i|-v|-d|-V|-h] [--] EXEC [ARGS]\n"
     "Trace the execution of EXEC on the given arguments ARGS\n"
     "\n"
     " -o FILE,--output FILE  write result to FILE\n"
     " -i,--intel             switch to intel syntax (default: at&t)\n"
     " -v,--verbose           verbose output\n"
     " -d,--debug             debug output\n"
     " -V,--version           display version and exit\n"
     " -h,--help              display this help\n";

  /* Parsing options */
  int optc;
  while ((optc = getopt_long (argc, argv, opts, long_opts, NULL)) != -1)
    switch (optc)
      {
      case 'o':         /* Output file */
        output = fopen (optarg, "we");
        if (!output)
	  			err (EXIT_FAILURE, "error: cannot open file '%s'", optarg);
        break;

      case 'i':         /* intel syntax mode */
        intel = true;
        break;

      case 'd':         /* Debug mode */
        debug = true;
        break;

      case 'v':         /* Verbosity mode */
        verbose = true;
        break;

      case 'V':         /* Display version number and exit */
        fprintf (stdout, "%s %s\n",
                 program_name, VERSION);
        fputs ("Trace the execution of a program on the given input\n", stdout);
        exit (EXIT_SUCCESS);
        break;

      case 'h':         /* Display usage and exit */
        fprintf (stdout, usage_msg, program_name);
        exit (EXIT_SUCCESS);
        break;

      default:
        errx (EXIT_FAILURE, "error: invalid option '%s'!", argv[optind - 1]);
      }

  /* Checking that extra arguments are present */
  if (optind > (argc - 1))
    errx (EXIT_FAILURE, "error: missing argument: an executable is required!");

  /* Extracting the complete argc/argv[] of the traced command */
	input = fopen (argv[optind], "r");
  if (input == NULL)
    errx (EXIT_FAILURE, "error: can't open the input file");

	int nb_line = 0;
  char str[MAX_LEN];
	while (fgets (str, MAX_LEN, input) != NULL)
    {
			if (str[0] != '\n')
				nb_line++;
		}
	rewind (input);

	cfg_t *cfg = NULL;
	cfg_t *cfg_entry = NULL;
  list_t *first_entry = NULL;
  list_t *tail_entries = NULL;
  uint16_t nb_function = 0;

	hashtable_t *ht = hashtable_new (DEFAULT_HASHTABLE_SIZE);

  fp = fopen("toto.gv", "w+");
  char name_node[128];
  Agraph_t *g;
  g = agopen ("G", Agstrictdirected, NULL);
	Agsym_t *sym;
	sym = agattr (g, AGNODE, "shape", "box");

	if (ht == NULL)
		err (EXIT_FAILURE, "error: cannot create hashtable");

	while (fgets (str, MAX_LEN, input) != NULL)
		{
			if (str[0] != '\n')
				{
					size_t line_length = strlen (str);
					char *exec_argv[line_length];
					char *token = strtok (str, " ");
					int index = 0;
					while (token != NULL)
						{
							size_t token_length = strlen (token);
							if (token[token_length - 1] == '\n')
								token[token_length - 1] = '\0'; /* Formatting trick */
							exec_argv[index] = token;
							index++;
							token = strtok (NULL, " ");
						}
					exec_argv[index] = NULL;
					int exec_argc = index;

				  /* Perfom various checks on the executable file */
				  arch_t exec_arch = check_execfile (exec_argv[0]);

				  /* Display the traced command */
				  fprintf (output, "%s: starting to trace '", program_name);
				  for (int i = 0; i < exec_argc - 1; i++)
				    fprintf (output, "%s ", exec_argv[i]);
				  fprintf (output, "%s'\n\n", exec_argv[exec_argc - 1]);

				  /* Forking and tracing */
				  pid_t child = fork ();
				  if (child == -1)
				    errx (EXIT_FAILURE, "error: fork failed!");

				  /* Initialized and start the child */
				  if (child == 0)
				    {
				      /* Disabling ASLR */
				      personality (ADDR_NO_RANDOMIZE);

				      /* Start tracing the process */
				      if (ptrace (PTRACE_TRACEME, 0, NULL, NULL) == -1)
								errx (EXIT_FAILURE,
					      			"error: cannot operate from inside a ptrace() call!");

				      /* Starting the traced executable */
				      execve (exec_argv[0], exec_argv, envp);

				    }

				  /* Parent process */
				  int status;
				  byte_t buf[MAX_OPCODE_BYTES];
				  uintptr_t ip;
				  struct user_regs_struct regs;

				  /* Initializing Capstone disassembler */
				  csh handle;
				  cs_insn *insn;
				  size_t count;

				  cs_mode exec_mode = 0;
				  switch (exec_arch)
				    {
				    case x86_32_arch:
				      exec_mode = CS_MODE_32;
				      break;

				    case x86_64_arch:
				      exec_mode = CS_MODE_64;
				      break;

				    default:
				      errx (EXIT_FAILURE,
										"error: '%s' unsupported architecture", exec_argv[0]);
				    }

				  /* Initialize the assembly decoder */
				  if (cs_open (CS_ARCH_X86, exec_mode, &handle) != CS_ERR_OK)
				    errx (EXIT_FAILURE, "error: cannot start capstone disassembler");

				  /* Set syntax flavor output */
				  if (intel)
				    cs_option (handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
				  else
				    cs_option (handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_ATT);

				  /* Main disassembling loop */
				  size_t instr_count = 0;
          stack_t *stack = NULL;

				  while (true)
				    {
				      /* Waiting for child process */
				      wait (&status);
				      if (WIFEXITED (status))
								break;

				      /* Get instruction pointer */
				      ptrace (PTRACE_GETREGS, child, NULL, &regs);

				      /* Printing instruction pointer */
				      ip = get_current_ip (&regs);
				      fprintf (output, "0x%" PRIxPTR "  ", ip);

				      /* Get the opcode from memory */
				      for (size_t i = 0; i < MAX_OPCODE_BYTES; i += 8)
								{
					  			long *ptr = (long *) &(buf[i]);
					  			*ptr = ptrace (PTRACE_PEEKDATA, child, ip + i, NULL);
								}

				      /* Get the mnemonic from decoder */
				      count = cs_disasm (handle, &(buf[0]), MAX_OPCODE_BYTES, 0x1000, 0, &insn);

							if (count > 0)
								{
					  			/* Display the bytes */
					  			for (size_t i = 0; i < insn[0].size; i++)
					    		       fprintf (output, " %02x", buf[i]);

					  			/* Pretty printing and formating */
					  			if (insn[0].size != 8 && insn[0].size != 11)
					    			fprintf (output, "\t");

					  			for (int i = 0; i < 4 - (insn[0].size / 3); i++)
					    			fprintf (output, "\t");

					  			/* Display mnemonic and operand */
					  			fprintf (output, "%s  %s", insn[0].mnemonic, insn[0].op_str);
					  			fprintf (output, "\n");



                  sprintf(name_node,"0x%" PRIxPTR  "  ", ip);
                  for (size_t i = 0; i < insn[0].size; i++)
                    sprintf(name_node + strlen(name_node), "%02x ", buf[i]);
                  sprintf(name_node + strlen(name_node), " %s ",insn[0].mnemonic);
                  sprintf(name_node + strlen(name_node), "%s",insn[0].op_str);

					  			/* Create the instr_t structure */
					  			instr_t *instr = instr_new (ip, insn[0].size, buf);
					  			if (!instr)
									{
										hashtable_delete (ht);
										cs_free (insn, count);
										cs_close (&handle);
										fclose (input);
										fclose (output);
					    			err (EXIT_FAILURE, "error: cannot create instruction");
									}
								cs_free (insn, count);

									if (!cfg)
									 	{
											/* Create a new trace and store it */
											cfg = cfg_new (ht, instr, name_node, &tail_entries);

											if (!cfg)
  											{
  												hashtable_delete (ht);
  												cs_close (&handle);
  												fclose (input);
  												fclose (output);
  									 			err (EXIT_FAILURE, "error: cannot create a control flow graph");
  											}
									 		cfg_entry = cfg;
                      first_entry = list_new (cfg);
                      tail_entries = first_entry;
										}
									else
										{
											/* Insert a new element in the cfg and update cfg to hold
											 * the new node */

											cfg = cfg_insert (ht, cfg, instr, name_node, &stack, &tail_entries, &nb_function);

											if (!cfg)
  											{
  												hashtable_delete (ht);
  												cs_close (&handle);
  												fclose (input);
  												fclose (output);
  												err (EXIT_FAILURE, "error: cannot create a control flow graph");
  											}
										}

					  			/* Updating counters */
					  			instr_count++;
								}

				      /* Continue to next instruction... */
				      /* Note that, sometimes, ptrace(PTRACE_SINGLESTEP) returns '-1'
				       * to notify that the child process did not respond quick enough,
				       * we have to wait for ptrace() to return '0'. */
				      while (ptrace(PTRACE_SINGLESTEP, child, NULL, NULL));
				    }
          stack_delete (stack);
          stack = NULL;
					cs_close (&handle);
				  fprintf(output,
					  "\n"
					  "\tStatistics about this run\n"
					  "\t=========================\n"
					  "* #instructions executed: %zu\n"
					  "* #unique instructions:   %zu\n"
					  "* #hashtable buckets:     %zu\n"
					  "* #hashtable collisions:  %zu\n\n\n",
					  instr_count, hashtable_entries (ht),
					  (size_t) DEFAULT_HASHTABLE_SIZE, hashtable_collisions (ht));
				}
		}

  g = graph_create_function(g, (cfg_t *) list_get_ith (first_entry, 90), NULL);

  fclose (input);
	fclose (output);

  list_delete (first_entry);
	hashtable_delete (ht);
  agwrite(g, fp);
  agclose(g);
	fclose (fp);
  return EXIT_SUCCESS;
}
