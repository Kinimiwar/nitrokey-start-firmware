/*
 * openpgp.c -- OpenPGP card protocol support
 *
 * Copyright (C) 2010 Free Software Initiative of Japan
 * Author: NIIBE Yutaka <gniibe@fsij.org>
 *
 * This file is a part of Gnuk, a GnuPG USB Token implementation.
 *
 * Gnuk is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Gnuk is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"
#include "ch.h"
#include "hal.h"
#include "gnuk.h"
#include "openpgp.h"
#include "polarssl/config.h"
#include "polarssl/sha1.h"

#define RSA_SIGNATURE_LENGTH 256 /* 256 byte == 2048-bit */

#define INS_VERIFY        			0x20
#define INS_CHANGE_REFERENCE_DATA		0x24
#define INS_PSO		  			0x2a
#define INS_RESET_RETRY_COUNTER			0x2c
#define INS_PGP_GENERATE_ASYMMETRIC_KEY_PAIR	0x47
#define INS_SELECT_FILE				0xa4
#define INS_READ_BINARY				0xb0
#define INS_GET_DATA				0xca
#define INS_PUT_DATA				0xda
#define INS_PUT_DATA_ODD			0xdb	/* For key import */

extern const char const select_file_TOP_result[20];
extern const char const get_data_rb_result[6];

void
write_res_apdu (const uint8_t *p, int len, uint8_t sw1, uint8_t sw2)
{
  res_APDU_size = 2 + len;
  if (len)
    memcpy (res_APDU, p, len);
  res_APDU[len] = sw1;
  res_APDU[len+1] = sw2;
}

#define FILE_NONE	-1
#define FILE_DF_OPENPGP	0
#define FILE_MF		1
#define FILE_EF_DIR	2
#define FILE_EF_SERIAL	3

static int file_selection = FILE_NONE;

static void
cmd_verify (void)
{
  int len;
  uint8_t p2 = cmd_APDU[3];
  int r;

  DEBUG_INFO (" - VERIFY\r\n");

  len = cmd_APDU[4];
  if (p2 == 0x81)
    r = verify_pso_cds (&cmd_APDU[5], len);
  else if (p2 == 0x82)
    r = verify_pso_other (&cmd_APDU[5], len);
  else
    r = verify_admin (&cmd_APDU[5], len);

  if (r < 0)
    GPG_SECURITY_FAILURE ();
  else if (r == 0)
    GPG_SECURITY_AUTH_BLOCKED ();
  else
    GPG_SUCCESS ();
}

int
gpg_change_keystring (int who_old, const uint8_t *old_ks,
		      int who_new, const uint8_t *new_ks)
{
  int r = gpg_do_load_prvkey (GPG_KEY_FOR_SIGNATURE, who_old, old_ks);

  if (r <= 0)
    return r;

  r = gpg_do_chks_prvkey (GPG_KEY_FOR_SIGNATURE, who_old, old_ks,
			  who_new, new_ks);
  if (r < 0)
    return -2;

  return r;
}

static void
cmd_change_password (void)
{
  uint8_t old_ks[KEYSTRING_MD_SIZE];
  uint8_t new_ks0[KEYSTRING_MD_SIZE+1];
  uint8_t *new_ks = &new_ks0[1];
  uint8_t p2 = cmd_APDU[3];
  int len = cmd_APDU[4];
  const uint8_t *pw = &cmd_APDU[5];
  const uint8_t *newpw;
  int pw_len, newpw_len;
  int who = p2 - 0x80;
  int r;

  if (who == 1)			/* PW1 */
    {
      const uint8_t *pk = gpg_do_read_simple (GNUK_DO_KEYSTRING_PW1);

      if (pk == NULL)
	{
	  if (len < 6)
	    {
	      GPG_SECURITY_FAILURE ();
	      return;
	    }

	  /* pk==NULL implies we have no prvkey */
	  pw_len = 6;
	  newpw = pw + pw_len;
	  newpw_len = len - pw_len;
	  goto no_prvkey;
	}
      else
	{
	  pw_len = pk[0];
	  newpw = pw + pw_len;
	  newpw_len = len - pw_len;
	}
    }
  else				/* PW3 (0x83) */
    {
      pw_len = verify_admin_0 (pw, len, -1);

      if (pw_len < 0)
	{
	  GPG_SECURITY_FAILURE ();
	  return;
	}
      else if (pw_len == 0)
	{
	  GPG_SECURITY_AUTH_BLOCKED ();
	  return;
	}
      else
	{
	  newpw = pw + pw_len;
	  newpw_len = len - pw_len;
	  gpg_set_pw3 (newpw, newpw_len);
	}
    }

  sha1 (pw, pw_len, old_ks);
  sha1 (newpw, newpw_len, new_ks);
  new_ks0[0] = newpw_len;

  r = gpg_change_keystring (who, old_ks, who, new_ks);
  if (r < -2)
    GPG_MEMORY_FAILURE ();
  else if (r < 0)
    GPG_SECURITY_FAILURE ();
  else if (r == 0 && who == 1)	/* no prvkey */
    {
    no_prvkey:
      gpg_do_write_simple (GNUK_DO_KEYSTRING_PW1, new_ks0, KEYSTRING_SIZE_PW1);
      reset_pso_cds ();
    }
  else if (r > 0 && who == 1)
    {
      gpg_do_write_simple (GNUK_DO_KEYSTRING_PW1, new_ks0, 1);
      reset_pso_cds ();
    }
  else				/* r >= 0 && who == 3 */
    GPG_SUCCESS ();
}

static void
cmd_reset_user_password (void)
{
  uint8_t p1 = cmd_APDU[2];
  int len = cmd_APDU[3];
  const uint8_t *pw = &cmd_APDU[4];
  const uint8_t *newpw;
  int pw_len, newpw_len;
  int r;

  if (p1 == 0x00)		/* by User with Reseting Code */
    {
      const uint8_t *pw_status_bytes = gpg_do_read_simple (GNUK_DO_PW_STATUS);
      const uint8_t *ks_rc = gpg_do_read_simple (GNUK_DO_KEYSTRING_RC);
      uint8_t old_ks[KEYSTRING_MD_SIZE];
      uint8_t new_ks0[KEYSTRING_MD_SIZE+1];
      uint8_t *new_ks = &new_ks0[1];

      if (pw_status_bytes == NULL
	  || pw_status_bytes[PW_STATUS_PW1] == 0) /* locked */
	{
	  GPG_SECURITY_AUTH_BLOCKED ();
	  return;
	}

      if (ks_rc == NULL)
	{
	  GPG_SECURITY_FAILURE ();
	  return;
	}

      pw_len = ks_rc[0];
      newpw = pw + pw_len;
      newpw_len = len - pw_len;
      sha1 (pw, pw_len, old_ks);
      sha1 (newpw, newpw_len, new_ks);
      new_ks0[0] = newpw_len;
      r = gpg_change_keystring (2, old_ks, 1, new_ks);
      if (r < -2)
	GPG_MEMORY_FAILURE ();
      else if (r < 0)
	{
	  uint8_t pwsb[SIZE_PW_STATUS_BYTES];

	sec_fail:
	  memcpy (pwsb, pw_status_bytes, SIZE_PW_STATUS_BYTES);
	  pwsb[PW_STATUS_RC]--;
	  gpg_do_write_simple (GNUK_DO_PW_STATUS, pwsb, SIZE_PW_STATUS_BYTES);
	  GPG_SECURITY_FAILURE ();
	}
      else if (r == 0)
	{
	  if (memcmp (ks_rc+1, old_ks, KEYSTRING_MD_SIZE) != 0)
	    goto sec_fail;
	  gpg_do_write_simple (GNUK_DO_KEYSTRING_PW1, new_ks0, KEYSTRING_SIZE_PW1);
	  reset_pso_cds ();
	}
      else
	{
	  reset_pso_cds ();
	  GPG_SUCCESS ();
	}
    }
  else				/* by Admin (p1 == 0x02) */
    {
      if (!ac_check_status (AC_ADMIN_AUTHORIZED))
	GPG_SECURITY_FAILURE ();
      else
	{
	  const uint8_t *old_ks = keystring_md_pw3;
	  uint8_t new_ks0[KEYSTRING_MD_SIZE+1];
	  uint8_t *new_ks = &new_ks0[1];

	  newpw_len = len;
	  newpw = pw;
	  sha1 (newpw, newpw_len, new_ks);
	  new_ks0[0] = newpw_len;
	  r = gpg_change_keystring (3, old_ks, 1, new_ks);
	  if (r < -2)
	    GPG_MEMORY_FAILURE ();
	  else if (r < 0)
	    GPG_SECURITY_FAILURE ();
	  else if (r == 0)
	    {
	      gpg_do_write_simple (GNUK_DO_KEYSTRING_PW1, new_ks0, KEYSTRING_SIZE_PW1);
	      reset_pso_cds ();
	    }
	  else
	    {
	      reset_pso_cds ();
	      GPG_SUCCESS ();
	    }
	}
    }
}

static void
cmd_put_data (void)
{
  uint8_t *data;
  uint16_t tag;
  int len;

  DEBUG_INFO (" - PUT DATA\r\n");

  if (file_selection != FILE_DF_OPENPGP)
    GPG_NO_RECORD();

  tag = ((cmd_APDU[2]<<8) | cmd_APDU[3]);
  len = cmd_APDU_size - 5;
  data = &cmd_APDU[5];
  if (len >= 256)
    /* extended Lc */
    {
      data += 2;
      len -= 2;
    }

  gpg_do_put_data (tag, data, len);
}

static void
cmd_pgp_gakp (void)
{
  DEBUG_INFO (" - Generate Asymmetric Key Pair\r\n");

  if (cmd_APDU[2] == 0x81)
    /* Get public key */
    gpg_do_public_key (cmd_APDU[5]);
  else
    {					/* Generate key pair */
      if (!ac_check_status (AC_ADMIN_AUTHORIZED))
	GPG_SECURITY_FAILURE ();

      /* XXX: Not yet supported */
      write_res_apdu (NULL, 0, 0x6a, 0x88); /* No record */
    }
}

static void
cmd_read_binary (void)
{
  DEBUG_INFO (" - Read binary\r\n");

  if (file_selection == FILE_EF_SERIAL)
    {
      if (cmd_APDU[3] >= 6)
	GPG_BAD_P0_P1 ();
      else
	/* Tag 5a, serial number */
	write_res_apdu ((const uint8_t *)get_data_rb_result,
			sizeof (get_data_rb_result), 0x90, 0x00);
    }
  else
    GPG_NO_RECORD();
}

static void
cmd_select_file (void)
{
  if (cmd_APDU[2] == 4)	/* Selection by DF name */
    {
      DEBUG_INFO (" - select DF by name\r\n");

      /*
       * P2 == 0, LC=6, name = D2 76 00 01 24 01
       */

      file_selection = FILE_DF_OPENPGP;

      /* XXX: Should return contents??? */
      GPG_SUCCESS ();
    }
  else if (cmd_APDU[4] == 2
	   && cmd_APDU[5] == 0x2f
	   && cmd_APDU[6] == 0x02)
    {
      DEBUG_INFO (" - select 0x2f02 EF\r\n");
      /*
       * MF.EF-GDO -- Serial number of the card and name of the owner
       */
      GPG_SUCCESS ();
      file_selection = FILE_EF_SERIAL;
    }
  else if (cmd_APDU[4] == 2
	   && cmd_APDU[5] == 0x3f
	   && cmd_APDU[6] == 0x00)
    {
      DEBUG_INFO (" - select ROOT MF\r\n");
      if (cmd_APDU[3] == 0x0c)
	{
	  GPG_SUCCESS ();
	}
      else
	{
	  write_res_apdu ((const uint8_t *)select_file_TOP_result,
			  sizeof (select_file_TOP_result), 0x90, 0x00);
	}

      file_selection = FILE_MF;
    }
  else
    {
      DEBUG_INFO (" - select ?? \r\n");

      file_selection = FILE_NONE;
      GPG_NO_FILE();
    }
}

static void
cmd_get_data (void)
{
  uint16_t tag = ((cmd_APDU[2]<<8) | cmd_APDU[3]);

  DEBUG_INFO (" - Get Data\r\n");

  if (file_selection != FILE_DF_OPENPGP)
    GPG_NO_RECORD();

  gpg_do_get_data (tag);
}

static void
cmd_pso (void)
{
  DEBUG_INFO (" - PSO\r\n");

  if (cmd_APDU[2] == 0x9E && cmd_APDU[3] == 0x9A)
    {
      if (!ac_check_status (AC_PSO_CDS_AUTHORIZED))
	{
	  GPG_SECURITY_FAILURE ();
	  return;
	}

      if (cmd_APDU_size != 8 + 35 && cmd_APDU_size != 8 + 35 + 1)
	/* Extended Lc: 3-byte */
	{
	  DEBUG_INFO (" wrong length: ");
	  DEBUG_SHORT (cmd_APDU_size);
	}
      else
	{
	  int len = (cmd_APDU[5]<<8) | cmd_APDU[6];
	  int r;

	  DEBUG_BYTE (len);  /* Should be cmd_APDU_size - 6 */

	  r = rsa_sign (&cmd_APDU[7], res_APDU, len);
	  if (r < 0)
	    /* XXX: fail code??? */
	    write_res_apdu (NULL, 0, 0x69, 0x85);
	  else
	    {			/* Success */
	      const uint8_t *pw_status_bytes = gpg_do_read_simple (GNUK_DO_PW_STATUS);

	      res_APDU[RSA_SIGNATURE_LENGTH] =  0x90;
	      res_APDU[RSA_SIGNATURE_LENGTH+1] =  0x00;
	      res_APDU_size = RSA_SIGNATURE_LENGTH + 2;

	      if (pw_status_bytes[0] == 0)
		reset_pso_cds ();

	      gpg_do_increment_digital_signature_counter ();
	    }
	}

      DEBUG_INFO ("done.\r\n");
    }
  else
    {				/* XXX: not yet supported */
      DEBUG_INFO (" - ??");
      DEBUG_BYTE (cmd_APDU[2]);
      DEBUG_INFO (" - ??");
      DEBUG_BYTE (cmd_APDU[3]);
      GPG_SUCCESS ();
    }
}

struct command
{
  uint8_t command;
  void (*cmd_handler) (void);
};

const struct command cmds[] = {
  { INS_VERIFY, cmd_verify },
  { INS_CHANGE_REFERENCE_DATA, cmd_change_password },
  { INS_PSO, cmd_pso },
  { INS_RESET_RETRY_COUNTER, cmd_reset_user_password },
  { INS_PGP_GENERATE_ASYMMETRIC_KEY_PAIR, cmd_pgp_gakp },
  { INS_SELECT_FILE, cmd_select_file },
  { INS_READ_BINARY, cmd_read_binary },
  { INS_GET_DATA, cmd_get_data },
  { INS_PUT_DATA, cmd_put_data },
  { INS_PUT_DATA_ODD, cmd_put_data },
};
#define NUM_CMDS ((int)(sizeof (cmds) / sizeof (struct command)))

static void
process_command_apdu (void)
{
  int i;
  uint8_t cmd = cmd_APDU[1];

  for (i = 0; i < NUM_CMDS; i++)
    if (cmds[i].command == cmd)
      break;

  if (i < NUM_CMDS)
    cmds[i].cmd_handler ();
  else
    {
      DEBUG_INFO (" - ??");
      DEBUG_BYTE (cmd);
      GPG_NO_INS ();
    }
}

Thread *gpg_thread;

msg_t
GPGthread (void *arg)
{
  (void)arg;

  gpg_thread = chThdSelf ();
  chEvtClear (ALL_EVENTS);

  while (1)
    {
      eventmask_t m;

      m = chEvtWaitOne (ALL_EVENTS);

      DEBUG_INFO ("GPG!\r\n");

      process_command_apdu ();

      chEvtSignal (icc_thread, EV_EXEC_FINISHED);
    }

  return 0;
}