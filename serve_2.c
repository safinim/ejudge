/* -*- mode: c -*- */
/* $Id$ */

/* Copyright (C) 2006-2010 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include "serve_state.h"
#include "runlog.h"
#include "prepare.h"
#include "l10n.h"
#include "html.h"
#include "errlog.h"
#include "protocol.h"
#include "clarlog.h"
#include "fileutl.h"
#include "teamdb.h"
#include "contests.h"
#include "job_packet.h"
#include "archive_paths.h"
#include "xml_utils.h"
#include "compile_packet.h"
#include "run_packet.h"
#include "curtime.h"
#include "userlist.h"
#include "sformat.h"
#include "misctext.h"
#include "charsets.h"
#include "compat.h"

#include <reuse/logger.h>
#include <reuse/xalloc.h>
#include <reuse/osdeps.h>
#include <reuse/exec.h>

#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>

#if CONF_HAS_LIBINTL - 0 == 1
#include <libintl.h>
#define _(x) gettext(x)
#else
#define _(x) x
#endif
#define __(x) x

#define ARMOR(s)  html_armor_buf(&ab, s)

void
serve_update_standings_file(serve_state_t state,
                            const struct contest_desc *cnts,
                            int force_flag)
{
  time_t start_time, stop_time, duration;
  int p = 0, charset_id = 0;

  run_get_times(state->runlog_state, &start_time, 0, &duration, &stop_time, 0);

  while (1) {
    if (state->global->is_virtual) break;
    if (force_flag) break;
    if (!state->global->autoupdate_standings) return;
    if (!duration) break;
    if (!state->global->board_fog_time) break;

    ASSERT(state->current_time >= start_time);
    ASSERT(state->global->board_fog_time >= 0);
    ASSERT(state->global->board_unfog_time >= 0);
    
    p = run_get_fog_period(state->runlog_state, state->current_time,
                           state->global->board_fog_time,
                           state->global->board_unfog_time);
    if (p == 1) return;
    break;
  }

  if (!state->global->is_virtual) {
    p = run_get_fog_period(state->runlog_state, state->current_time,
                           state->global->board_fog_time,
                           state->global->board_unfog_time);
  }
  charset_id = charset_get_id(state->global->standings_charset);
  l10n_setlocale(state->global->standings_locale_id);
  write_standings(state, cnts, state->global->status_dir,
                  state->global->standings_file_name,
                  state->global->users_on_page,
                  state->global->stand_header_txt,
                  state->global->stand_footer_txt,
                  state->accepting_mode, 0, charset_id);
  if (state->global->stand2_file_name[0]) {
    charset_id = charset_get_id(state->global->stand2_charset);
    write_standings(state, cnts, state->global->status_dir,
                    state->global->stand2_file_name, 0,
                    state->global->stand2_header_txt,
                    state->global->stand2_footer_txt,
                    state->accepting_mode, 0, charset_id);
  }
  l10n_setlocale(0);
  if (state->global->is_virtual) return;
  switch (p) {
  case 0:
    state->global->start_standings_updated = 1;
    break;
  case 1:
    state->global->fog_standings_updated = 1;
    break;
  case 2:
    state->global->unfog_standings_updated = 1;
    break;
  }
}

void
serve_update_public_log_file(serve_state_t state,
                             const struct contest_desc *cnts)
{
  time_t start_time, stop_time, duration;
  int p, charset_id = 0;

  if (!state->global->plog_update_time) return;
  if (state->current_time < state->last_update_public_log + state->global->plog_update_time) return;

  run_get_times(state->runlog_state, &start_time, 0, &duration, &stop_time, 0);

  while (1) {
    if (!duration) break;
    if (!state->global->board_fog_time) break;

    ASSERT(state->current_time >= start_time);
    ASSERT(state->global->board_fog_time >= 0);
    ASSERT(state->global->board_unfog_time >= 0);
    
    p = run_get_fog_period(state->runlog_state, state->current_time,
                           state->global->board_fog_time,
                           state->global->board_unfog_time);
    if (p == 1) return;
    break;
  }

  charset_id = charset_get_id(state->global->plog_charset);
  l10n_setlocale(state->global->standings_locale_id);
  write_public_log(state, cnts, state->global->status_dir,
                   state->global->plog_file_name,
                   state->global->plog_header_txt,
                   state->global->plog_footer_txt,
                   charset_id);
  state->last_update_public_log = state->current_time;
  l10n_setlocale(0);
}

static void
do_update_xml_log(serve_state_t state, const struct contest_desc *cnts,
                  char const *name, int external_mode)
{
  struct run_header rhead;
  int rtotal;
  const struct run_entry *rentries;
  path_t path1;
  path_t path2;
  FILE *fout;

  run_get_header(state->runlog_state, &rhead);
  rtotal = run_get_total(state->runlog_state);
  rentries = run_get_entries_ptr(state->runlog_state);

  snprintf(path1, sizeof(path1), "%s/in/%s.tmp",state->global->status_dir,name);
  snprintf(path2, sizeof(path2), "%s/dir/%s", state->global->status_dir, name);

  if (!(fout = fopen(path1, "w"))) {
    err("update_xml_log: cannot open %s", path1);
    return;
  }
  unparse_runlog_xml(state, cnts, fout, &rhead, rtotal, rentries,
                     external_mode, 0, state->current_time);
  if (ferror(fout)) {
    err("update_xml_log: write error");
    fclose(fout);
    unlink(path1);
    return;
  }
  if (fclose(fout) < 0) {
    err("update_xml_log: write error");
    unlink(path1);
    return;
  }
  if (rename(path1, path2) < 0) {
    err("update_xml_log: rename %s -> %s failed", path1, path2);
    unlink(path1);
    return;
  }
}

void
serve_update_external_xml_log(serve_state_t state,
                              const struct contest_desc *cnts)
{
  if (!state->global->external_xml_update_time) return;
  if (state->current_time < state->last_update_external_xml_log + state->global->external_xml_update_time) return;
  state->last_update_external_xml_log = state->current_time;
  do_update_xml_log(state, cnts, "external.xml", 1);
}

void
serve_update_internal_xml_log(serve_state_t state,
                              const struct contest_desc *cnts)
{
  if (!state->global->internal_xml_update_time) return;
  if (state->current_time < state->last_update_internal_xml_log + state->global->internal_xml_update_time) return;
  state->last_update_internal_xml_log = state->current_time;
  do_update_xml_log(state, cnts, "internal.xml", 0);
}

int
serve_update_status_file(serve_state_t state, int force_flag)
{
  struct prot_serve_status_v2 status;
  time_t t1, t2, t3, t4, t5;
  int p;

  if (!force_flag && state->current_time <= state->last_update_status_file) return 0;

  memset(&status, 0, sizeof(status));
  status.magic = PROT_SERVE_STATUS_MAGIC_V2;

  status.cur_time = state->current_time;
  run_get_times(state->runlog_state, &t1, &t2, &t3, &t4, &t5);
  status.start_time = t1;
  status.sched_time = t2;
  status.duration = t3;
  status.stop_time = t4;
  status.total_runs = run_get_total(state->runlog_state);
  status.total_clars = clar_get_total(state->clarlog_state);
  status.clars_disabled = state->global->disable_clars;
  status.team_clars_disabled = state->global->disable_team_clars;
  status.score_system = state->global->score_system;
  status.clients_suspended = state->clients_suspended;
  status.testing_suspended = state->testing_suspended;
  status.download_interval = state->global->team_download_time / 60;
  status.is_virtual = state->global->is_virtual;
  status.continuation_enabled = state->global->enable_continue;
  status.printing_enabled = state->global->enable_printing;
  status.printing_suspended = state->printing_suspended;
  status.always_show_problems = state->global->always_show_problems;
  status.accepting_mode = state->accepting_mode;
  status.testing_finished = state->testing_finished;

  status.upsolving_mode = state->upsolving_mode;
  status.freeze_standings = state->freeze_standings;
  status.view_source = state->view_source;
  status.view_protocol = state->view_protocol;
  status.full_protocol = state->full_protocol;
  status.disable_clars = state->disable_clars;

  if (status.start_time && status.duration && state->global->board_fog_time > 0
      && !status.is_virtual) {
    status.freeze_time = status.start_time + status.duration - state->global->board_fog_time;
    if (status.freeze_time < status.start_time) {
      status.freeze_time = status.start_time;
    }
  }
  status.finish_time = t5;
  //if (status.duration) status.continuation_enabled = 0;

  if (!state->global->is_virtual) {
    p = run_get_fog_period(state->runlog_state, state->current_time,
                           state->global->board_fog_time, state->global->board_unfog_time);
    if (p == 1 && state->global->autoupdate_standings) {
      status.standings_frozen = 1;
    }
  }

  status.stat_reported_before = state->stat_reported_before;
  status.stat_report_time = state->stat_report_time;

  status.max_online_time = state->max_online_time;
  status.max_online_count = state->max_online_count;

  memcpy(status.prob_prio, state->prob_prio, sizeof(status.prob_prio));

  generic_write_file((char*) &status, sizeof(status), SAFE,
                     state->global->status_dir, "status", "");
  state->last_update_status_file = state->current_time;
  return 1;
}

void
serve_load_status_file(serve_state_t state)
{
  struct prot_serve_status_v2 status;
  size_t stat_len = 0;
  char *ptr = 0;

  if (generic_read_file(&ptr, 0, &stat_len, 0, state->global->status_dir,
                        "dir/status", "") < 0) {
    if (state->global->score_system == SCORE_OLYMPIAD)
      state->accepting_mode = 1;
    return;
  }
  if (stat_len != sizeof(status)) {
    info("load_status_file: length %zu does not match %zu",
         stat_len, sizeof(status));
    xfree(ptr);
    if (state->global->score_system == SCORE_OLYMPIAD)
      state->accepting_mode = 1;
    return;
  }
  memcpy(&status, ptr, sizeof(status));
  xfree(ptr);
  if (status.magic != PROT_SERVE_STATUS_MAGIC_V2) {
    info("load_status_file: bad magic value");
    if (state->global->score_system == SCORE_OLYMPIAD)
      state->accepting_mode = 1;
    return;
  }

  state->clients_suspended = status.clients_suspended;
  info("load_status_file: clients_suspended = %d", state->clients_suspended);
  state->testing_suspended = status.testing_suspended;
  info("load_status_file: testing_suspended = %d", state->testing_suspended);
  state->accepting_mode = status.accepting_mode;
  if (state->global->score_system == SCORE_OLYMPIAD
      && state->global->is_virtual) {
    state->accepting_mode = 1;
  }
  info("load_status_file: accepting_mode = %d", state->accepting_mode);
  state->printing_suspended = status.printing_suspended;
  info("load_status_file: printing_suspended = %d", state->printing_suspended);
  state->stat_reported_before = status.stat_reported_before;
  state->stat_report_time = status.stat_report_time;

  state->upsolving_mode = status.upsolving_mode;
  info("load_status_file: upsolving_mode = %d", state->upsolving_mode);
  state->freeze_standings = status.freeze_standings;
  state->view_source = status.view_source;
  state->view_protocol = status.view_protocol;
  state->full_protocol = status.full_protocol;
  state->disable_clars = status.disable_clars;
  state->testing_finished = status.testing_finished;

  state->max_online_time = status.max_online_time;
  state->max_online_count = status.max_online_count;

  memcpy(state->prob_prio, status.prob_prio, sizeof(state->prob_prio));
}

int
serve_check_user_quota(serve_state_t state, int user_id, size_t size)
{
  int num;
  size_t total;

  if (size > state->global->max_run_size) return -1;
  run_get_team_usage(state->runlog_state, user_id, &num, &total);
  if (num > state->global->max_run_num
      || total + size > state->global->max_run_total)
    return -1;
  return 0;
}

int
serve_check_clar_quota(serve_state_t state, int user_id, size_t size)
{
  int num;
  size_t total;

  if (size > state->global->max_clar_size) return -1;
  clar_get_user_usage(state->clarlog_state, user_id, &num, &total);
  if (num > state->global->max_clar_num
      || total + size > state->global->max_clar_total)
    return -1;
  return 0;
}

int
serve_check_cnts_caps(serve_state_t state,
                      const struct contest_desc *cnts,
                      int user_id, int bit)
{
  opcap_t caps;
  unsigned char const *login = 0;

  login = teamdb_get_login(state->teamdb_state, user_id);
  if (!login || !*login) return 0;

  if (opcaps_find(&cnts->capabilities, login, &caps) < 0) return 0;
  if (opcaps_check(caps, bit) < 0) return 0;
  return 1;
}

int
serve_get_cnts_caps(serve_state_t state,
                    const struct contest_desc *cnts,
                    int user_id, opcap_t *out_caps)
{
  opcap_t caps;
  unsigned char const *login = 0;

  login = teamdb_get_login(state->teamdb_state, user_id);
  if (!login || !*login) return -1;

  if (opcaps_find(&cnts->capabilities, login, &caps) < 0) return -1;
  if (out_caps) *out_caps = caps;
  return 0;
}

static int
do_build_compile_dirs(serve_state_t state,
                      const unsigned char *status_dir,
                      const unsigned char *report_dir)
{
  int i;

  if (!status_dir || !*status_dir || !report_dir || !*report_dir) abort();

  for (i = 0; i < state->compile_dirs_u; i++)
    if (!strcmp(state->compile_dirs[i].status_dir, status_dir))
      break;
  if (i < state->compile_dirs_u) return i;

  if (state->compile_dirs_u == state->compile_dirs_a) {
    if (!state->compile_dirs_a) state->compile_dirs_a = 8;
    state->compile_dirs_a *= 2;
    XREALLOC(state->compile_dirs, state->compile_dirs_a);
  }

  state->compile_dirs[state->compile_dirs_u].status_dir = xstrdup(status_dir);
  state->compile_dirs[state->compile_dirs_u].report_dir = xstrdup(report_dir);
  return state->compile_dirs_u++;
}

void
serve_build_compile_dirs(serve_state_t state)
{
  int i;

  for (i = 1; i <= state->max_lang; i++) {
    if (!state->langs[i]) continue;
    do_build_compile_dirs(state,
                          state->langs[i]->compile_status_dir,
                          state->langs[i]->compile_report_dir);
  }
}

static int
do_build_run_dirs(serve_state_t state,
                  const unsigned char *status_dir,
                  const unsigned char *report_dir,
                  const unsigned char *team_report_dir,
                  const unsigned char *full_report_dir)
{
  int i;

  if (!status_dir || !*status_dir) abort();

  for (i = 0; i < state->run_dirs_u; i++)
    if (!strcmp(state->run_dirs[i].status_dir, status_dir))
      break;
  if (i < state->run_dirs_u) return i;

  if (state->run_dirs_u == state->run_dirs_a) {
    if (!state->run_dirs_a) state->run_dirs_a = 8;
    state->run_dirs_a *= 2;
    XREALLOC(state->run_dirs, state->run_dirs_a);
  }

  state->run_dirs[state->run_dirs_u].status_dir = xstrdup(status_dir);
  state->run_dirs[state->run_dirs_u].report_dir = xstrdup(report_dir);
  state->run_dirs[state->run_dirs_u].team_report_dir = xstrdup(team_report_dir);
  state->run_dirs[state->run_dirs_u].full_report_dir = xstrdup(full_report_dir);
  return state->run_dirs_u++;
}

void
serve_build_run_dirs(serve_state_t state)
{
  int i;

  for (i = 1; i <= state->max_tester; i++) {
    if (!state->testers[i]) continue;
    do_build_run_dirs(state, state->testers[i]->run_status_dir,
                      state->testers[i]->run_report_dir,
                      state->testers[i]->run_team_report_dir,
                      state->testers[i]->run_full_archive_dir);
  }
}

int
serve_create_symlinks(serve_state_t state)
{
  unsigned char src_path[PATH_MAX];
  unsigned char dst_path[PATH_MAX];
  path_t stand_file;
  int npages, pgn;

  if (state->global->stand_symlink_dir[0] && state->global->htdocs_dir[0]) {
    if (state->global->users_on_page > 0) {
      // FIXME: check, that standings_file_name depends on page number
      npages = (teamdb_get_total_teams(state->teamdb_state)
                + state->global->users_on_page - 1)
        / state->global->users_on_page;
      for (pgn = 0; pgn < npages; pgn++) {
        if (!pgn) {
          snprintf(stand_file, sizeof(stand_file),
                   state->global->standings_file_name, pgn + 1);
        } else {
          snprintf(stand_file, sizeof(stand_file),
                   state->global->stand_file_name_2, pgn + 1);
        }
        snprintf(src_path, sizeof(src_path), "%s/dir/%s",
                 state->global->status_dir, stand_file);
        snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
                 state->global->htdocs_dir, state->global->stand_symlink_dir,
                 stand_file);
        os_normalize_path(dst_path);
        if (unlink(dst_path) < 0 && errno != ENOENT) {
          err("unlink %s failed: %s", dst_path, os_ErrorMsg());
          //return -1;
        }
        if (symlink(src_path, dst_path) < 0) {
          err("symlink %s->%s failed: %s", dst_path, src_path, os_ErrorMsg());
          //return -1;
        }
      }
    } else {
      snprintf(src_path, sizeof(src_path), "%s/dir/%s",
               state->global->status_dir, state->global->standings_file_name);
      snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
               state->global->htdocs_dir, state->global->stand_symlink_dir,
               state->global->standings_file_name);
      os_normalize_path(dst_path);
      if (unlink(dst_path) < 0 && errno != ENOENT) {
        err("unlink %s failed: %s", dst_path, os_ErrorMsg());
        //return -1;
      }
      if (symlink(src_path, dst_path) < 0) {
        err("symlink %s->%s failed: %s", dst_path, src_path, os_ErrorMsg());
        //return -1;
      }
    }
  }
  if (state->global->stand2_symlink_dir[0] && state->global->htdocs_dir[0]
      && state->global->stand2_file_name[0]) {
    snprintf(src_path, sizeof(src_path), "%s/dir/%s",
             state->global->status_dir, state->global->stand2_file_name);
    snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
             state->global->htdocs_dir, state->global->stand2_symlink_dir,
             state->global->stand2_file_name);
    os_normalize_path(dst_path);
    if (unlink(dst_path) < 0 && errno != ENOENT) {
      err("unlink %s failed: %s", dst_path, os_ErrorMsg());
      //return -1;
    }
    if (symlink(src_path, dst_path) < 0) {
      err("symlink %s->%s failed: %s", dst_path, src_path, os_ErrorMsg());
      //return -1;
    }
  }
  if (state->global->plog_symlink_dir[0] && state->global->htdocs_dir[0]
      && state->global->plog_file_name[0]
      && state->global->plog_update_time > 0) {
    snprintf(src_path, sizeof(src_path), "%s/dir/%s",
             state->global->status_dir, state->global->plog_file_name);
    snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
             state->global->htdocs_dir, state->global->plog_symlink_dir,
             state->global->plog_file_name);
    os_normalize_path(dst_path);
    if (unlink(dst_path) < 0 && errno != ENOENT) {
      err("unlink %s failed: %s", dst_path, os_ErrorMsg());
      //return -1;
    }
    if (symlink(src_path, dst_path) < 0) {
      err("symlink %s->%s failed: %s", dst_path, src_path, os_ErrorMsg());
      //return -1;
    }
  }
  return 0;
}

const unsigned char *
serve_get_email_sender(const struct contest_desc *cnts)
{
  int sysuid;
  struct passwd *ppwd;

  if (cnts && cnts->register_email) return cnts->register_email;
  sysuid = getuid();
  ppwd = getpwuid(sysuid);
  return ppwd->pw_name;
}

static void
generate_statistics_email(
        serve_state_t state,
        const struct contest_desc *cnts,
        time_t from_time,
        time_t to_time,
        int utf8_mode)
{
  unsigned char esubj[1024];
  struct tm *ptm;
  char *etxt = 0, *ftxt = 0;
  size_t elen = 0, flen = 0;
  FILE *eout = 0, *fout = 0;
  const unsigned char *mail_args[7];
  const unsigned char *originator;
  struct tm tm1;

  ptm = localtime(&from_time);
  snprintf(esubj, sizeof(esubj),
           "Daily statistics for %04d/%02d/%02d, contest %d",
           ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
           state->global->contest_id);

  eout = open_memstream(&etxt, &elen);
  generate_daily_statistics(state, eout, from_time, to_time, utf8_mode);
  close_memstream(eout); eout = 0;
  if (!etxt || !*etxt) {
    xfree(etxt);
    return;
  }

  localtime_r(&from_time, &tm1);

  fout = open_memstream(&ftxt, &flen);
  fprintf(fout,
          "Hello,\n"
          "\n"
          "This is daily report for contest %d (%s)\n"
          "Report day: %04d/%02d/%02d\n\n"
          "%s\n\n"
          "-\n"
          "Regards,\n"
          "the ejudge contest management system\n",
          state->global->contest_id, cnts->name,
          tm1.tm_year + 1900, tm1.tm_mon + 1, tm1.tm_mday,
          etxt);
  close_memstream(fout); fout = 0;

  originator = serve_get_email_sender(cnts);
  mail_args[0] = "mail";
  mail_args[1] = "";
  mail_args[2] = esubj;
  mail_args[3] = originator;
  mail_args[4] = cnts->daily_stat_email;
  mail_args[5] = ftxt;
  mail_args[6] = 0;
  send_job_packet(NULL, (unsigned char **) mail_args, 0);
  xfree(ftxt); ftxt = 0;
  xfree(etxt); etxt = 0;
}

void
serve_check_stat_generation(
        serve_state_t state,
        const struct contest_desc *cnts,
        int force_flag,
        int utf8_mode)
{
  struct tm *ptm;
  time_t thisday, nextday;

  if (!cnts) return;
  if (!force_flag && state->stat_last_check_time > 0
      && state->stat_last_check_time + 600 > state->current_time)
    return;
  state->stat_last_check_time = state->current_time;
  if (!cnts->daily_stat_email) return;

  if (!state->stat_reported_before) {
    // set the time to the beginning of this day
    ptm = localtime(&state->current_time);
    ptm->tm_hour = 0;
    ptm->tm_min = 0;
    ptm->tm_sec = 0;
    ptm->tm_isdst = -1;
    if ((thisday = mktime(ptm)) == (time_t) -1) {
      err("check_stat_generation: mktime() failed");
      thisday = 0;
    }
    state->stat_reported_before = thisday;
  }
  if (!state->stat_report_time) {
    // set the time to the beginning of the next day
    ptm = localtime(&state->current_time);
    ptm->tm_hour = 0;
    ptm->tm_min = 0;
    ptm->tm_sec = 0;
    ptm->tm_isdst = -1;
    ptm->tm_mday++;             // pretty valid. see man mktime
    if ((nextday = mktime(ptm)) == (time_t) -1) {
      err("check_stat_generation: mktime() failed");
      nextday = 0;
    }
    state->stat_report_time = nextday;
  }

  if (state->current_time < state->stat_report_time) return;

  // set the stat_report_time to the beginning of today
  ptm = localtime(&state->current_time);
  ptm->tm_hour = 0;
  ptm->tm_min = 0;
  ptm->tm_sec = 0;
  ptm->tm_isdst = -1;
  if ((thisday = mktime(ptm)) != (time_t) -1)
    state->stat_report_time = thisday;

  // generate report for each day from stat_reported_before to stat_report_time
  thisday = state->stat_reported_before;
  while (thisday < state->stat_report_time) {
    ptm = localtime(&thisday);
    ptm->tm_hour = 0;
    ptm->tm_min = 0;
    ptm->tm_sec = 0;
    ptm->tm_isdst = -1;
    ptm->tm_mday++;
    if ((nextday = mktime(ptm)) == (time_t) -1) {
      err("check_stat_generation: mktime() failed");
      state->stat_reported_before = 0;
      state->stat_report_time = 0;
      return;
    }
    generate_statistics_email(state, cnts, thisday, nextday, utf8_mode);
    thisday = nextday;
  }

  ptm = localtime(&thisday);
  ptm->tm_hour = 0;
  ptm->tm_min = 0;
  ptm->tm_sec = 0;
  ptm->tm_isdst = -1;
  ptm->tm_mday++;
  if ((nextday = mktime(ptm)) == (time_t) -1) {
    err("check_stat_generation: mktime() failed");
    state->stat_reported_before = 0;
    state->stat_report_time = 0;
    return;
  }
  state->stat_reported_before = thisday;
  state->stat_report_time = nextday;
}

void
serve_move_files_to_insert_run(serve_state_t state, int run_id)
{
  int total = run_get_total(state->runlog_state);
  int i, s;
  const struct section_global_data *global = state->global;

  ASSERT(run_id >= 0 && run_id < total);
  // the last run
  if (run_id == total - 1) return;
  for (i = total - 2; i >= run_id; i--) {
    info("rename: %d -> %d", i, i + 1);
    archive_remove(state, global->run_archive_dir, i + 1, 0);
    archive_remove(state, global->xml_report_archive_dir, i + 1, 0);
    archive_remove(state, global->report_archive_dir, i + 1, 0);
    if (global->team_enable_rep_view) {
      archive_remove(state, global->team_report_archive_dir, i + 1, 0);
    }
    if (global->enable_full_archive) {
      archive_remove(state, global->full_archive_dir, i + 1, 0);
    }
    archive_remove(state, global->audit_log_dir, i + 1, 0);

    archive_rename(state, global->audit_log_dir, 0, i, 0, i + 1, 0, 0);
    serve_audit_log(state, i + 1, 0, 0, 0,
                    "Command: rename\n"
                    "From-run-id: %d\n"
                    "To-run-id: %d\n", i, i + 1);

    s = run_get_status(state->runlog_state, i + 1);
    archive_rename(state, global->run_archive_dir, 0, i, 0, i + 1, 0, 0);
    if (s >= RUN_PSEUDO_FIRST && s <= RUN_PSEUDO_LAST) continue;
    if (s == RUN_IGNORED || s == RUN_DISQUALIFIED || s ==RUN_PENDING) continue;
    if (run_is_imported(state->runlog_state, i + 1)) continue;
    archive_rename(state, global->xml_report_archive_dir, 0, i, 0, i + 1, 0, 0);
    archive_rename(state, global->report_archive_dir, 0, i, 0, i + 1, 0, 0);
    if (global->team_enable_rep_view) {
      archive_rename(state, global->team_report_archive_dir, 0,i,0,i + 1,0,0);
    }
    if (global->enable_full_archive) {
      archive_rename(state, global->full_archive_dir, 0, i, 0, i + 1, 0, 0);
    }
  }
}

void
serve_audit_log(serve_state_t state, int run_id, int user_id,
                ej_ip_t ip, int ssl_flag, const char *format, ...)
{
  unsigned char buf[16384];
  unsigned char tbuf[128];
  va_list args;
  struct tm *ltm;
  path_t audit_path;
  FILE *f;
  unsigned char *login;
  size_t buf_len;

  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  buf_len = strlen(buf);
  while (buf_len > 0 && isspace(buf[buf_len - 1])) buf[--buf_len] = 0;

  ltm = localtime(&state->current_time);
  snprintf(tbuf, sizeof(tbuf), "%04d/%02d/%02d %02d:%02d:%02d",
           ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
           ltm->tm_hour, ltm->tm_min, ltm->tm_sec);

  archive_make_write_path(state, audit_path, sizeof(audit_path),
                          state->global->audit_log_dir, run_id, 0, 0);
  if (archive_dir_prepare(state, state->global->audit_log_dir,
                          run_id, 0, 1) < 0) return;
  if (!(f = fopen(audit_path, "a"))) return;

  fprintf(f, "Date: %s\n", tbuf);
  if (!user_id) {
    fprintf(f, "From: SYSTEM\n");
  } else if (user_id <= 0) {
    fprintf(f, "From: unauthentificated user\n");
  } else if (!(login = teamdb_get_login(state->teamdb_state, user_id))){
    fprintf(f, "From: user %d (login unknown)\n", user_id);
  } else {
    fprintf(f, "From: %s (uid %d)\n", login, user_id);
  }
  if (ip) {
    fprintf(f, "Ip: %s%s\n", xml_unparse_ip(ip), ssl_flag?"/SSL":"");
  }
  fprintf(f, "%s\n\n", buf);

  fclose(f);
}

static const unsigned char b32_digits[]=
"0123456789ABCDEFGHIJKLMNOPQRSTUV";
static void
b32_number(unsigned long long num, unsigned char buf[])
{
  int i;

  memset(buf, '0', EJ_SERVE_PACKET_NAME_SIZE - 1);
  buf[EJ_SERVE_PACKET_NAME_SIZE - 1] = 0;
  i = EJ_SERVE_PACKET_NAME_SIZE - 2;
  while (num > 0 && i >= 0) {
    buf[i] = b32_digits[num & 0x1f];
    i--;
    num >>= 5;
  }
  ASSERT(!num);
}

void
serve_packet_name(int run_id, int prio, unsigned char buf[])
{
  unsigned long long num = 0;
  struct timeval ts;

  // generate "random" number, that would include the
  // pid of "serve", the current time (with microseconds)
  // and some small random component.
  // pid is 2 byte (15 bit)
  // run_id is 2 byte
  // time_t component - 4 byte
  // nanosec component - 4 byte

  // EJ_SERVE_PACKET_NAME_SIZE == 13
  // total packet name bits: 60 (12 * 5)

  //OLD:
  // 6666555555555544444444443333333333222222222211111111110000000000
  // 3210987654321098765432109876543210987654321098765432109876543210
  //     ==P==
  //          =====run_id====
  //                         ======pid======
  //                                        ===========time==========

  /*
  num = (getpid() & 0x7fffLLU) << 25LLU;
  num |= (run_id & 0x7fffLLU) << 40LLU;
  gettimeofday(&ts, 0);
  num |= (ts.tv_sec ^ ts.tv_usec) & 0x1ffffff;
  b32_number(num, buf);
  prio += 16;
  if (prio < 0) prio = 0;
  if (prio > 31) prio = 31;
  buf[0] = b32_digits[prio];
  */

  //NEW:
  // 6666555555555544444444443333333333222222222211111111110000000000
  // 3210987654321098765432109876543210987654321098765432109876543210
  //     ==P==
  //          =====run_id=========
  //                              =======time=========
  //                                                  ======usec=====

  prio += 16;
  if (prio < 0) prio = 0;
  if (prio > 31) prio = 31;
  gettimeofday(&ts, 0);

  num = prio;
  num <<= 55;
  num |= (run_id & 0xfffffLLU) << 35;
  num |= (ts.tv_sec & 0xfffffLLU) << 15;
  num |= (ts.tv_usec & 0x7fffLLU);
  b32_number(num, buf);
}

int
serve_compile_request(
        serve_state_t state,
        unsigned char const *str,
        int len,
        int run_id,
        int user_id,
        int lang_id,
        int locale_id,
        int output_only,
        unsigned char const *sfx,
        char **compiler_env,
        int accepting_mode,
        int priority_adjustment,
        int notify_flag,
        const struct section_problem_data *prob,
        const struct section_language_data *lang)
{
  struct compile_run_extra rx;
  struct compile_request_packet cp;
  void *pkt_buf = 0;
  size_t pkt_len = 0;
  unsigned char pkt_name[EJ_SERVE_PACKET_NAME_SIZE];
  int arch_flags;
  path_t run_arch;
  const struct section_global_data *global = state->global;
  path_t tmp_path, tmp_path_2;
  char *src_header_text = 0, *src_footer_text = 0, *src_text = 0;
  size_t src_header_size = 0, src_footer_size = 0, src_size = 0;
  unsigned char *src_out_text = 0;
  size_t src_out_size = 0;
  int prio = 0;

  if (prob->source_header[0]) {
    sformat_message(tmp_path, sizeof(tmp_path), 0, prob->source_header,
                    global, prob, lang, 0, 0, 0, 0, 0);
    if (os_IsAbsolutePath(tmp_path)) {
      snprintf(tmp_path_2, sizeof(tmp_path_2), "%s", tmp_path);
    } else if (state->global->advanced_layout > 0) {
      get_advanced_layout_path(tmp_path_2, sizeof(tmp_path_2),
                               state->global, prob, tmp_path, -1);
    } else {
      snprintf(tmp_path_2, sizeof(tmp_path_2), "%s/%s",
               global->statement_dir, tmp_path);
    }
    if (generic_read_file(&src_header_text, 0, &src_header_size, 0, 0,
                          tmp_path_2, "") < 0)
      goto failed;
  }
  if (prob->source_footer[0]) {
    sformat_message(tmp_path, sizeof(tmp_path), 0, prob->source_footer,
                    global, prob, lang, 0, 0, 0, 0, 0);
    if (os_IsAbsolutePath(tmp_path)) {
      snprintf(tmp_path_2, sizeof(tmp_path_2), "%s", tmp_path);
    } else if (state->global->advanced_layout > 0) {
      get_advanced_layout_path(tmp_path_2, sizeof(tmp_path_2),
                               state->global, prob, tmp_path, -1);
    } else {
      snprintf(tmp_path_2, sizeof(tmp_path_2), "%s/%s",
               global->statement_dir, tmp_path);
    }
    if (generic_read_file(&src_footer_text, 0, &src_footer_size, 0, 0,
                          tmp_path_2, "") < 0)
      goto failed;
  }

  if (accepting_mode == -1) accepting_mode = state->accepting_mode;

  if (!state->compile_request_id) state->compile_request_id++;

  memset(&cp, 0, sizeof(cp));
  cp.judge_id = state->compile_request_id++;
  cp.contest_id = global->contest_id;
  cp.run_id = run_id;
  cp.lang_id = lang_id;
  cp.locale_id = locale_id;
  cp.output_only = output_only;
  get_current_time(&cp.ts1, &cp.ts1_us);
  cp.run_block_len = sizeof(rx);
  cp.run_block = &rx;
  cp.env_num = -1;
  cp.env_vars = (unsigned char**) compiler_env;
  if (lang->style_checker_cmd[0]) {
    cp.style_checker = (unsigned char*) lang->style_checker_cmd;
  }

  memset(&rx, 0, sizeof(rx));
  rx.accepting_mode = accepting_mode;
  rx.priority_adjustment = priority_adjustment;
  rx.notify_flag = notify_flag;

  if (compile_request_packet_write(&cp, &pkt_len, &pkt_buf) < 0) {
    // FIXME: need reasonable recovery?
    goto failed;
  }

  prio = 0;
  if (lang) prio += lang->priority_adjustment;
  if (prob) prio += prob->priority_adjustment;
  prio += find_user_priority_adjustment(state, user_id);
  prio += priority_adjustment;
  if (prob && prob->id < EJ_SERVE_STATE_TOTAL_PROBS)
    prio += state->prob_prio[prob->id];

  if (!sfx) sfx = "";
  serve_packet_name(run_id, prio, pkt_name);

  if (src_header_size > 0 || src_footer_size > 0) {
    if (len < 0) {
      arch_flags = archive_make_read_path(state, run_arch, sizeof(run_arch),
                                          global->run_archive_dir, run_id, 0,0);
      if (arch_flags < 0) goto failed;
      if (generic_read_file(&src_text, 0, &src_size, arch_flags, 0,
                            run_arch, "") < 0)
        goto failed;
      str = src_text;
      len = src_size;
    }
    src_out_size = src_header_size + len + src_footer_size;
    src_out_text = (unsigned char*) xmalloc(src_out_size + 1);
    if (src_header_size > 0)
      memcpy(src_out_text, src_header_text, src_header_size);
    if (len > 0)
      memcpy(src_out_text + src_header_size, str, len);
    if (src_footer_size > 0)
      memcpy(src_out_text + src_header_size + len, src_footer_text,
             src_footer_size);
    if (generic_write_file(src_out_text, src_out_size, 0,
                           global->compile_src_dir, pkt_name, sfx) < 0)
      goto failed;
  } else if (len < 0) {
    // copy from archive
    arch_flags = archive_make_read_path(state, run_arch, sizeof(run_arch),
                                        global->run_archive_dir, run_id, 0,0);
    if (arch_flags < 0) goto failed;
    if (generic_copy_file(arch_flags, 0, run_arch, "",
                          0, lang->compile_src_dir, pkt_name, sfx) < 0)
      goto failed;
  } else {
    // write from memory
    if (generic_write_file(str, len, 0,
                           lang->compile_src_dir, pkt_name, sfx) < 0)
      goto failed;
  }

  if (generic_write_file(pkt_buf, pkt_len, SAFE,
                         lang->compile_queue_dir, pkt_name, "") < 0) {
    goto failed;
  }

  if (run_change_status(state->runlog_state, run_id, RUN_COMPILING, 0, -1,
                        cp.judge_id) < 0) {
    goto failed;
  }

  xfree(pkt_buf);
  xfree(src_header_text);
  xfree(src_footer_text);
  xfree(src_text);
  xfree(src_out_text);
  return 0;

 failed:
  xfree(pkt_buf);
  xfree(src_header_text);
  xfree(src_footer_text);
  xfree(src_text);
  xfree(src_out_text);
  return -1;
}

int
serve_run_request(
        serve_state_t state,
        FILE *errf,
        const unsigned char *run_text,
        size_t run_size,
        int run_id,
        int user_id,
        int prob_id,
        int lang_id,
        int variant,
        int priority_adjustment,
        int judge_id,
        int accepting_mode,
        int notify_flag,
        const unsigned char *compile_report_dir,
        const struct compile_reply_packet *comp_pkt)
{
  int cn;
  struct section_problem_data *prob;
  struct section_language_data *lang = 0;
  unsigned char *arch = "", *exe_sfx = "";
  const unsigned char *user_name;
  int prio, i;
  unsigned char pkt_base[EJ_SERVE_PACKET_NAME_SIZE];
  unsigned char exe_out_name[256];
  unsigned char exe_in_name[256];
  struct run_request_packet *run_pkt = 0;
  struct teamdb_export te;
  void *run_pkt_out = 0;
  size_t run_pkt_out_size = 0;
  struct userlist_user_info *ui = 0;

  path_t run_exe_dir;
  path_t run_queue_dir;

  if (prob_id <= 0 || prob_id > state->max_prob 
      || !(prob = state->probs[prob_id])) {
    fprintf(errf, "invalid problem %d", prob_id);
    return -1;
  }
  if (lang_id > 0) {
    if (lang_id > state->max_lang || !(lang = state->langs[lang_id])) {
      fprintf(errf, "invalid language %d", lang_id);
      return -1;
    }
  }
  if (!(user_name = teamdb_get_name(state->teamdb_state, user_id))) {
    fprintf(errf, "invalid user %d", user_id);
    return -1;
  }
  if (!*user_name) user_name = teamdb_get_login(state->teamdb_state, user_id);

  if (lang) arch = lang->arch;
  if (lang) exe_sfx = lang->exe_sfx;

  cn = find_tester(state, prob_id, arch);
  if (cn < 1 || cn > state->max_tester || !state->testers[cn]) {
    fprintf(errf, "no appropriate checker for <%s>, <%s>\n",
            prob->short_name, arch);
    return -1;
  }

  if (state->testers[cn]->run_dir[0]) {
    snprintf(run_exe_dir, sizeof(run_exe_dir),
             "%s/exe", state->testers[cn]->run_dir);
    snprintf(run_queue_dir, sizeof(run_queue_dir),
             "%s/queue", state->testers[cn]->run_dir);
  } else {
    snprintf(run_exe_dir, sizeof(run_exe_dir), "%s/exe", 
             state->global->run_dir);
    snprintf(run_queue_dir, sizeof(run_queue_dir), "%s/queue",
             state->global->run_dir);
  }

  if (prob->variant_num <= 0 && variant > 0) {
    fprintf(errf, "variant is not allowed for this problem\n");
    return -1;
  }
  if (prob->variant_num > 0) {
    if (variant <= 0) variant = find_variant(state, user_id, prob_id, 0);
    if (variant <= 0) {
      fprintf(errf, "no appropriate variant for <%s>, <%s>\n",
              user_name, prob->short_name);
      return -1;
    }
  }

  /* calculate a priority */
  prio = 0;
  if (lang) prio += lang->priority_adjustment;
  prio += prob->priority_adjustment;
  prio += find_user_priority_adjustment(state, user_id);
  prio += state->testers[cn]->priority_adjustment;
  prio += priority_adjustment;
  if (prob_id < EJ_SERVE_STATE_TOTAL_PROBS)
    prio += state->prob_prio[prob_id];
  
  if (judge_id < 0) {
    if (!state->compile_request_id) state->compile_request_id++;
    judge_id = state->compile_request_id++;
  }
  if (accepting_mode < 0) {
    if (state->global->score_system == SCORE_OLYMPIAD
        && state->global->is_virtual > 0) {
      accepting_mode = 1;
    } else {
      accepting_mode = state->accepting_mode;
    }
  }

  /* generate a packet name */
  serve_packet_name(run_id, prio, pkt_base);
  snprintf(exe_out_name, sizeof(exe_out_name), "%s%s", pkt_base, exe_sfx);

  if (!run_text) {
    snprintf(exe_in_name, sizeof(exe_in_name), "%06d%s", run_id, exe_sfx);
    if (generic_copy_file(REMOVE, compile_report_dir, exe_in_name, "",
                          0, state->global->run_exe_dir,exe_out_name, "") < 0) {
      fprintf(errf, "copying failed");
      return -1;
    }
  } else {
    if (generic_write_file(run_text, run_size, 0,
                           state->global->run_exe_dir, exe_out_name, "") < 0) {
      fprintf(errf, "writing failed");
      return -1;
    }
  }

  /* create an internal representation of run packet */
  XALLOCAZ(run_pkt, 1);

  run_pkt->judge_id = judge_id;
  run_pkt->contest_id = state->global->contest_id;
  run_pkt->run_id = run_id;
  run_pkt->problem_id = prob->tester_id;
  run_pkt->accepting_mode = accepting_mode;
  run_pkt->scoring_system = state->global->score_system;
  run_pkt->variant = variant;
  run_pkt->accept_partial = prob->accept_partial;
  run_pkt->user_id = user_id;
  run_pkt->disable_sound = state->global->disable_sound;
  run_pkt->full_archive = state->global->enable_full_archive;
  run_pkt->memory_limit = state->global->enable_memory_limit_error;
  run_pkt->secure_run = state->global->secure_run;
  run_pkt->notify_flag = notify_flag;
  run_pkt->advanced_layout = state->global->advanced_layout;
  if (run_pkt->secure_run && prob->disable_security) run_pkt->secure_run = 0;
  if (run_pkt->secure_run && lang && lang->disable_security) run_pkt->secure_run = 0;
  run_pkt->security_violation = state->global->detect_violations;
  get_current_time(&run_pkt->ts4, &run_pkt->ts4_us);
  if (comp_pkt) {
    run_pkt->ts1 = comp_pkt->ts1;
    run_pkt->ts1_us = comp_pkt->ts1_us;
    run_pkt->ts2 = comp_pkt->ts2;
    run_pkt->ts2_us = comp_pkt->ts2_us;
    run_pkt->ts3 = comp_pkt->ts3;
    run_pkt->ts3_us = comp_pkt->ts3_us;
  } else {
    run_pkt->ts3 = run_pkt->ts4;
    run_pkt->ts3_us = run_pkt->ts4_us;
    run_pkt->ts2 = run_pkt->ts4;
    run_pkt->ts2_us = run_pkt->ts4_us;
    run_pkt->ts1 = run_pkt->ts4;
    run_pkt->ts1_us = run_pkt->ts4_us;
  }
  run_pkt->exe_sfx = exe_sfx;
  run_pkt->arch = arch;

  // process language-specific milliseconds time adjustments
  if (prob->lang_time_adj_millis) {
    size_t lsn = strlen(lang->short_name);
    size_t vl;
    int adj, n;
    unsigned char *sn;
    for (i = 0; (sn = prob->lang_time_adj_millis[i]); i++) {
      vl = strlen(sn);
      if (vl > lsn + 1
          && !strncmp(sn, lang->short_name, lsn)
          && sn[lsn] == '='
          && sscanf(sn + lsn + 1, "%d%n", &adj, &n) == 1
          && !sn[lsn + 1 + n]
          && adj >= 0
          && adj <= 1000000) {
        run_pkt->time_limit_adj_millis = adj;
      }
    }
  }

  // process language-specific time adjustments
  if (prob->lang_time_adj) {
    size_t lsn = strlen(lang->short_name);
    size_t vl;
    int adj, n;
    unsigned char *sn;
    for (i = 0; (sn = prob->lang_time_adj[i]); i++) {
      vl = strlen(sn);
      if (vl > lsn + 1
          && !strncmp(sn, lang->short_name, lsn)
          && sn[lsn] == '='
          && sscanf(sn + lsn + 1, "%d%n", &adj, &n) == 1
          && !sn[lsn + 1 + n]
          && adj >= 0
          && adj <= 100) {
        run_pkt->time_limit_adj = adj;
      }
    }
  }

  /* in new binary packet format we don't care about neither "special"
   * characters in spellings nor about spelling length
   */
  teamdb_export_team(state->teamdb_state, user_id, &te);
  ui = 0;
  if (te.user) ui = te.user->cnts0;
  if (ui && ui->spelling && ui->spelling[0]) {
    run_pkt->user_spelling = ui->spelling;
  }
  if (!run_pkt->user_spelling && ui && ui->name
      && ui->name[0]) {
    run_pkt->user_spelling = ui->name;
  }
  if (!run_pkt->user_spelling && te.login && te.user->login
      && te.user->login[0]) {
    run_pkt->user_spelling = te.user->login;
  }
  /* run_pkt->user_spelling is allowed to be NULL */

  if (prob->spelling[0]) {
    run_pkt->prob_spelling = prob->spelling;
  }
  if (!run_pkt->prob_spelling) {
    run_pkt->prob_spelling = prob->short_name;
  }
  /* run_pkt->prob_spelling is allowed to be NULL */

  /* generate external representation of the packet */
  if (run_request_packet_write(run_pkt, &run_pkt_out_size, &run_pkt_out) < 0) {
    fprintf(errf, "run_request_packet_write failed\n");
    return -1;
  }

  if (generic_write_file(run_pkt_out, run_pkt_out_size, SAFE,
                         state->global->run_queue_dir, pkt_base, "") < 0) {
    xfree(run_pkt_out);
    fprintf(errf, "failed to write run packet\n");
    return -1;
  }

  /* update status */
  xfree(run_pkt_out); run_pkt_out = 0;
  if (run_change_status(state->runlog_state, run_id, RUN_RUNNING, 0, -1,
                        judge_id) < 0) {
    return -1;
  }

  return 0;
}

void
serve_send_clar_notify_email(serve_state_t state,
                             const struct contest_desc *cnts,
                             int user_id, const unsigned char *user_name,
                             const unsigned char *subject,
                             const unsigned char *text)
{
  unsigned char esubj[1024];
  FILE *fmsg = 0;
  char *ftxt = 0;
  size_t flen = 0;
  const unsigned char *originator = 0;
  const unsigned char *mail_args[7];

  if (!state || !cnts || !cnts->clar_notify_email) return;

  snprintf(esubj, sizeof(esubj), "New clar request in contest %d", cnts->id);
  originator = serve_get_email_sender(cnts);
  fmsg = open_memstream(&ftxt, &flen);
  fprintf(fmsg, "Hello,\n\nNew clarification request is received\n"
          "Contest: %d (%s)\n"
          "User: %d (%s)\n"
          "Subject: %s\n\n"
          "%s\n\n-\n"
          "Regards,\n"
          "the ejudge contest management system\n",
          cnts->id, cnts->name, user_id, user_name, subject, text);
  close_memstream(fmsg); fmsg = 0;
  mail_args[0] = "mail";
  mail_args[1] = "";
  mail_args[2] = esubj;
  mail_args[3] = originator;
  mail_args[4] = cnts->clar_notify_email;
  mail_args[5] = ftxt;
  mail_args[6] = 0;
  send_job_packet(NULL, (unsigned char**) mail_args, 0);
  xfree(ftxt); ftxt = 0;
}

void
serve_send_check_failed_email(const struct contest_desc *cnts, int run_id)
{
  unsigned char esubj[1024];
  const unsigned char *originator = 0;
  FILE *fmsg = 0;
  char *ftxt = 0;
  size_t flen = 0;
  const unsigned char *mail_args[7];

  if (!cnts->cf_notify_email) return;

  snprintf(esubj, sizeof(esubj), "Check failed in contest %d", cnts->id);
  originator = serve_get_email_sender(cnts);

  fmsg = open_memstream(&ftxt, &flen);
  fprintf(fmsg, "Hello,\n\nRun evaluation got \"Check failed\"!\n"
          "Contest: %d (%s)\n"
          "Run Id: %d\n\n-\n"
          "Regards,\n"
          "the ejudge contest management system\n",
          cnts->id, cnts->name, run_id);
  close_memstream(fmsg); fmsg = 0;
  mail_args[0] = "mail";
  mail_args[1] = "";
  mail_args[2] = esubj;
  mail_args[3] = originator;
  mail_args[4] = cnts->cf_notify_email;
  mail_args[5] = ftxt;
  mail_args[6] = 0;
  send_job_packet(NULL, (unsigned char **) mail_args, 0);
  xfree(ftxt); ftxt = 0;
}

void
serve_send_email_to_user(
        const struct contest_desc *cnts,
        const serve_state_t cs,
        int user_id,
        const unsigned char *subject,
        const unsigned char *text)
{
  const struct userlist_user *u = 0;
  const unsigned char *originator = 0;
  const unsigned char *mail_args[7];

  if (!(u = teamdb_get_userlist(cs->teamdb_state, user_id))) return;
  if (!u->email || !strchr(u->email, '@')) return;

  originator = serve_get_email_sender(cnts);

  mail_args[0] = "mail";
  mail_args[1] = "";
  mail_args[2] = subject;
  mail_args[3] = originator;
  mail_args[4] = u->email;
  mail_args[5] = text;
  mail_args[6] = 0;
  send_job_packet(NULL, (unsigned char**) mail_args, 0);
}

void
serve_notify_user_run_status_change(
        const struct contest_desc *cnts,
        const serve_state_t cs,
        int user_id,
        int run_id,
        int new_status)
{
  unsigned char nsubj[1024];
  FILE *msg_f = 0;
  char *msg_t = 0;
  size_t msg_z = 0;

  // FIXME: do not send a notification if the user is online?

  if (cnts->default_locale_num > 0)
    l10n_setlocale(cnts->default_locale_num);
  snprintf(nsubj, sizeof(nsubj),
           _("Status of your submit %d is changed in contest %d"),
           run_id, cnts->id);
  msg_f = open_memstream(&msg_t, &msg_z);
  fprintf(msg_f, _("Status of your submit is changed\n"));
  fprintf(msg_f, _("Contest: %d (%s)\n"), cnts->id, cnts->name);
  fprintf(msg_f, "Run Id: %d\n", run_id);
  fprintf(msg_f, _("New status: %s\n"),
          run_status_str(new_status, 0, 0, 0, 0));
  if (cnts->team_url) {
    fprintf(msg_f, "URL: %s?contest_id=%d&login=%s\n", cnts->team_url,
            cnts->id, teamdb_get_login(cs->teamdb_state, user_id));
  }
  fprintf(msg_f, "\n-\nRegards,\nthe ejudge contest management system (www.ejudge.ru)\n");
  close_memstream(msg_f); msg_f = 0;
  if (cnts->default_locale_num > 0) {
    l10n_setlocale(cnts->default_locale_num);
  }
  serve_send_email_to_user(cnts, cs, user_id, nsubj, msg_t);
  xfree(msg_t); msg_t = 0; msg_z = 0;
}

int
serve_read_compile_packet(serve_state_t state,
                          const struct contest_desc *cnts,
                          const unsigned char *compile_status_dir,
                          const unsigned char *compile_report_dir,
                          const unsigned char *pname)
{
  unsigned char rep_path[PATH_MAX];
  int  r, rep_flags = 0;
  struct run_entry re;

  char *comp_pkt_buf = 0;       /* need char* for generic_read_file */
  size_t comp_pkt_size = 0;
  struct compile_reply_packet *comp_pkt = 0;
  long report_size = 0;
  unsigned char errmsg[1024] = { 0 };
  unsigned char *team_name = 0;
  struct compile_run_extra *comp_extra = 0;
  struct section_problem_data *prob = 0;
  struct section_language_data *lang = 0;

  if ((r = generic_read_file(&comp_pkt_buf, 0, &comp_pkt_size, SAFE | REMOVE,
                             compile_status_dir, pname, "")) <= 0)
    return r;

  if (compile_reply_packet_read(comp_pkt_size, comp_pkt_buf, &comp_pkt) < 0) {
    /* failed to parse a compile packet */
    /* we can't do any reasonable recovery, just drop the packet */
    goto non_fatal_error;
  }
  if (comp_pkt->contest_id != cnts->id) {
    err("read_compile_packet: mismatched contest_id %d", comp_pkt->contest_id);
    goto non_fatal_error;
  }
  if (run_get_entry(state->runlog_state, comp_pkt->run_id, &re) < 0) {
    err("read_compile_packet: invalid run_id %d", comp_pkt->run_id);
    goto non_fatal_error;
  }
  if (comp_pkt->judge_id != re.judge_id) {
    err("read_compile_packet: judge_id mismatch: %d, %d", comp_pkt->judge_id,
        re.judge_id);
    goto non_fatal_error;
  }
  if (re.status != RUN_COMPILING) {
    err("read_compile_packet: run %d is not compiling", comp_pkt->run_id);
    goto non_fatal_error;
  }

  comp_extra = (typeof(comp_extra)) comp_pkt->run_block;
  if (!comp_extra || comp_pkt->run_block_len != sizeof(*comp_extra)
      || comp_extra->accepting_mode < 0 || comp_extra->accepting_mode > 1) {
    snprintf(errmsg, sizeof(errmsg), "invalid run block\n");
    goto report_check_failed;
  }

  if (comp_pkt->status == RUN_CHECK_FAILED
      || comp_pkt->status == RUN_COMPILE_ERR
      || comp_pkt->status == RUN_STYLE_ERR) {
    if ((report_size = generic_file_size(compile_report_dir, pname, "")) < 0) {
      err("read_compile_packet: cannot get report file size");
      snprintf(errmsg, sizeof(errmsg), "cannot get size of %s/%s\n",
               compile_report_dir, pname);
      goto report_check_failed;
    }

    rep_flags = archive_make_write_path(state, rep_path, sizeof(rep_path),
                                        state->global->xml_report_archive_dir,
                                        comp_pkt->run_id, report_size, 0);
    if (rep_flags < 0) {
      snprintf(errmsg, sizeof(errmsg),
               "archive_make_write_path: %s, %d, %ld failed\n",
               state->global->xml_report_archive_dir, comp_pkt->run_id,
               report_size);
      goto report_check_failed;
    }
  }

  if (comp_pkt->status == RUN_CHECK_FAILED) {
    /* if status change fails, we cannot do reasonable recovery */
    if (run_change_status(state->runlog_state, comp_pkt->run_id,
                          RUN_CHECK_FAILED, 0, -1, 0) < 0)
      goto non_fatal_error;
    if (archive_dir_prepare(state, state->global->xml_report_archive_dir,
                            comp_pkt->run_id, 0, 0) < 0)
      goto non_fatal_error;
    if (generic_copy_file(REMOVE, compile_report_dir, pname, "",
                          rep_flags, 0, rep_path, "") < 0) {
      snprintf(errmsg, sizeof(errmsg),
               "generic_copy_file: %s, %s, %d, %s failed\n",
               compile_report_dir, pname, rep_flags, rep_path);
      goto report_check_failed;
    }
    serve_send_check_failed_email(cnts, comp_pkt->run_id);
    goto success;
  }

  if (comp_pkt->status == RUN_COMPILE_ERR
      || comp_pkt->status == RUN_STYLE_ERR) {
    /* if status change fails, we cannot do reasonable recovery */
    if (run_change_status(state->runlog_state, comp_pkt->run_id,
                          comp_pkt->status, 0, -1, 0) < 0)
      goto non_fatal_error;

    if (archive_dir_prepare(state, state->global->xml_report_archive_dir,
                            comp_pkt->run_id, 0, 0) < 0) {
      snprintf(errmsg, sizeof(errmsg), "archive_dir_prepare: %s, %d failed\n",
               state->global->xml_report_archive_dir, comp_pkt->run_id);
      goto report_check_failed;
    }
    if (generic_copy_file(REMOVE, compile_report_dir, pname, "",
                          rep_flags, 0, rep_path, "") < 0) {
      snprintf(errmsg, sizeof(errmsg), "generic_copy_file: %s, %s, %d, %s failed\n",
               compile_report_dir, pname, rep_flags, rep_path);
      goto report_check_failed;
    }
    serve_update_standings_file(state, cnts, 0);
    if (state->global->notify_status_change > 0 && !re.is_hidden
        && comp_extra->notify_flag) {
      serve_notify_user_run_status_change(cnts, state, re.user_id,
                                          comp_pkt->run_id, comp_pkt->status);
    }
    goto success;
  }

  /* check run parameters */
  if (re.prob_id < 1 || re.prob_id > state->max_prob
      || !(prob = state->probs[re.prob_id])) {
    snprintf(errmsg, sizeof(errmsg), "invalid problem %d\n", re.prob_id);
    goto report_check_failed;
  }
  if (re.lang_id < 1 || re.lang_id > state->max_lang
      || !(lang = state->langs[re.lang_id])) {
    snprintf(errmsg, sizeof(errmsg), "invalid language %d\n", re.lang_id);
    goto report_check_failed;
  }
  if (!(team_name = teamdb_get_name(state->teamdb_state, re.user_id))) {
    snprintf(errmsg, sizeof(errmsg), "invalid team %d\n", re.user_id);
    goto report_check_failed;
  }
  if (prob->disable_testing && prob->enable_compilation > 0) {
    if (run_change_status(state->runlog_state, comp_pkt->run_id, RUN_ACCEPTED,
                          0, -1, comp_pkt->judge_id) < 0)
      goto non_fatal_error;
    if (state->global->notify_status_change > 0 && !re.is_hidden
        && comp_extra->notify_flag) {
      serve_notify_user_run_status_change(cnts, state, re.user_id,
                                          comp_pkt->run_id, RUN_ACCEPTED);
    }
    goto success;
  }

  if (run_change_status(state->runlog_state, comp_pkt->run_id, RUN_COMPILED,
                        0, -1, comp_pkt->judge_id) < 0)
    goto non_fatal_error;

  /*
   * so far compilation is successful, and now we prepare a run packet
   */

  if (serve_run_request(state, stderr, 0, 0, comp_pkt->run_id, re.user_id,
                        re.prob_id, re.lang_id, 0,
                        comp_extra->priority_adjustment,
                        comp_pkt->judge_id, comp_extra->accepting_mode,
                        comp_extra->notify_flag, compile_report_dir,
                        comp_pkt) < 0) {
    snprintf(errmsg, sizeof(errmsg), "failed to write run packet\n");
    goto report_check_failed;
  }

 success:
  xfree(comp_pkt_buf);
  compile_reply_packet_free(comp_pkt);
  return 1;

 report_check_failed:
  serve_send_check_failed_email(cnts, comp_pkt->run_id);

  /* this is error recover, so if error happens again, we cannot do anything */
  if (run_change_status(state->runlog_state, comp_pkt->run_id,
                        RUN_CHECK_FAILED, 0, -1, 0) < 0)
    goto non_fatal_error;
  report_size = strlen(errmsg);
  rep_flags = archive_make_write_path(state, rep_path, sizeof(rep_path),
                                      state->global->xml_report_archive_dir,
                                      comp_pkt->run_id, report_size, 0);
  if (archive_dir_prepare(state, state->global->xml_report_archive_dir,
                          comp_pkt->run_id, 0, 0) < 0)
    goto non_fatal_error;
  /* error code is ignored */
  generic_write_file(errmsg, report_size, rep_flags, 0, rep_path, 0);
  /* goto non_fatal_error; */

 non_fatal_error:
  xfree(comp_pkt_buf);
  compile_reply_packet_free(comp_pkt);
  return 0;
}

int
serve_is_valid_status(serve_state_t state, int status, int mode)
{
  if (state->global->score_system == SCORE_OLYMPIAD) {
    switch (status) {
    case RUN_OK:
    case RUN_PARTIAL:
    case RUN_RUN_TIME_ERR:
    case RUN_TIME_LIMIT_ERR:
    case RUN_PRESENTATION_ERR:
    case RUN_WRONG_ANSWER_ERR:
    case RUN_ACCEPTED:
    case RUN_CHECK_FAILED:
    case RUN_MEM_LIMIT_ERR:
    case RUN_SECURITY_ERR:
      return 1;
    case RUN_COMPILE_ERR:
    case RUN_STYLE_ERR:
    case RUN_FULL_REJUDGE:
    case RUN_REJUDGE:
    case RUN_IGNORED:
    case RUN_DISQUALIFIED:
    case RUN_PENDING:
      if (mode != 1) return 0;
      return 1;
    default:
      return 0;
    }
  } else if (state->global->score_system == SCORE_KIROV) {
    switch (status) {
    case RUN_OK:
    case RUN_PARTIAL:
    case RUN_CHECK_FAILED:
    case RUN_ACCEPTED:
      return 1;
    case RUN_COMPILE_ERR:
    case RUN_STYLE_ERR:
    case RUN_REJUDGE:
    case RUN_IGNORED:
    case RUN_DISQUALIFIED:
    case RUN_PENDING:
      if (mode != 1) return 0;
      return 1;
    default:
      return 0;
    }
  } else {
    switch (status) {
    case RUN_OK:
    case RUN_ACCEPTED:
    case RUN_RUN_TIME_ERR:
    case RUN_TIME_LIMIT_ERR:
    case RUN_PRESENTATION_ERR:
    case RUN_WRONG_ANSWER_ERR:
    case RUN_CHECK_FAILED:
    case RUN_MEM_LIMIT_ERR:
    case RUN_SECURITY_ERR:
      return 1;
    case RUN_COMPILE_ERR:
    case RUN_STYLE_ERR:
    case RUN_REJUDGE:
    case RUN_IGNORED:
    case RUN_DISQUALIFIED:
    case RUN_PENDING:
      if (mode != 1) return 0;
      return 1;
    default:
      return 0;
    }
  }
}

static unsigned char *
time_to_str(unsigned char *buf, size_t size, int secs, int usecs)
{
  struct tm *ltm;
  time_t tt = secs;

  if (secs <= 0) {
    snprintf(buf, size, "N/A");
    return buf;
  }
  ltm = localtime(&tt);
  snprintf(buf, size, "%04d/%02d/%02d %02d:%02d:%02d.%06d",
           ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
           ltm->tm_hour, ltm->tm_min, ltm->tm_sec, usecs);
  return buf;
}

static unsigned char *
dur_to_str(unsigned char *buf, size_t size, int sec1, int usec1,
           int sec2, int usec2)
{
  long long d;

  if (sec1 <= 0 || sec2 <= 0) {
    snprintf(buf, size, "N/A");
    return buf;
  }
  if ((d = sec2 * 1000000 + usec2 - (sec1 * 1000000 + usec1)) < 0) {
    snprintf(buf, size, "t1 > t2");
    return buf;
  }
  d = (d + 500) / 1000;
  snprintf(buf, size, "%lld.%03lld", d / 1000, d % 1000);
  return buf;
}

int
serve_read_run_packet(serve_state_t state,
                      const struct contest_desc *cnts,
                      const unsigned char *run_status_dir,
                      const unsigned char *run_report_dir,
                      const unsigned char *run_full_archive_dir,
                      const unsigned char *pname)
{
  path_t rep_path, full_path;
  int r, rep_flags, rep_size, full_flags;
  struct run_entry re;
  char *reply_buf = 0;          /* need char* for generic_read_file */
  size_t reply_buf_size = 0;
  struct run_reply_packet *reply_pkt = 0;
  char *audit_text = 0;
  size_t audit_text_size = 0;
  FILE *f = 0;
  int ts8, ts8_us;
  unsigned char time_buf[64];

  get_current_time(&ts8, &ts8_us);
  if ((r = generic_read_file(&reply_buf, 0, &reply_buf_size, SAFE | REMOVE,
                             run_status_dir, pname, "")) <= 0)
    return r;

  if (run_reply_packet_read(reply_buf_size, reply_buf, &reply_pkt) < 0)
    goto failed;
  xfree(reply_buf), reply_buf = 0;

  if (reply_pkt->contest_id != cnts->id) {
    err("read_run_packet: contest_id mismatch: %d in packet",
        reply_pkt->contest_id);
    goto failed;
  }
  if (run_get_entry(state->runlog_state, reply_pkt->run_id, &re) < 0) {
    err("read_run_packet: invalid run_id: %d", reply_pkt->run_id);
    goto failed;
  }
  if (re.status != RUN_RUNNING) {
    err("read_run_packet: run %d status is not RUNNING", reply_pkt->run_id);
    goto failed;
  }
  if (re.judge_id != reply_pkt->judge_id) {
    err("read_run_packet: judge_id mismatch: packet: %d, db: %d",
        reply_pkt->judge_id, re.judge_id);
    goto failed;
  }

  if (!serve_is_valid_status(state, reply_pkt->status, 2))
    goto bad_packet_error;

  if (state->global->score_system == SCORE_OLYMPIAD) {
    if (re.prob_id < 1 || re.prob_id > state->max_prob
        || !state->probs[re.prob_id])
      goto bad_packet_error;
  } else if (state->global->score_system == SCORE_KIROV) {
    /*
    if (status != RUN_PARTIAL && status != RUN_OK
        && status != RUN_CHECK_FAILED) goto bad_packet_error;
    */
    if (re.prob_id < 1 || re.prob_id > state->max_prob
        || !state->probs[re.prob_id])
      goto bad_packet_error;
    if (reply_pkt->score < 0
        || reply_pkt->score > state->probs[re.prob_id]->full_score)
      goto bad_packet_error;
    /*
    for (n = 0; n < serve_state.probs[re.prob_id]->dp_total; n++)
      if (re.timestamp < serve_state.probs[re.prob_id]->dp_infos[n].deadline)
        break;
    if (n < serve_state.probs[re.prob_id]->dp_total) {
      score += serve_state.probs[re.prob_id]->dp_infos[n].penalty;
      if (score > serve_state.probs[re.prob_id]->full_score)
        score = serve_state.probs[re.prob_id]->full_score;
      if (score < 0) score = 0;
    }
    */
  } else if (state->global->score_system == SCORE_MOSCOW) {
    if (re.prob_id < 1 || re.prob_id > state->max_prob
        || !state->probs[re.prob_id])
      goto bad_packet_error;
    if (reply_pkt->score < 0
        || reply_pkt->score > state->probs[re.prob_id]->full_score)
      goto bad_packet_error;
  } else {
    reply_pkt->score = -1;
  }
  if (re.prob_id >= 1 && re.prob_id <= state->max_prob
      && state->probs[re.prob_id] && state->probs[re.prob_id]->use_ac_not_ok
      && reply_pkt->status == RUN_OK) {
    reply_pkt->status = RUN_ACCEPTED;
  }
  if (reply_pkt->status == RUN_CHECK_FAILED)
    serve_send_check_failed_email(cnts, reply_pkt->run_id);
  if (reply_pkt->marked_flag < 0) reply_pkt->marked_flag = 0;
  if (run_change_status_2(state->runlog_state, reply_pkt->run_id,
                          reply_pkt->status, reply_pkt->failed_test,
                          reply_pkt->score, 0, reply_pkt->marked_flag) < 0)
    goto failed;
  serve_update_standings_file(state, cnts, 0);
  if (state->global->notify_status_change > 0 && !re.is_hidden
      && reply_pkt->notify_flag) {
    serve_notify_user_run_status_change(cnts, state, re.user_id,
                                        reply_pkt->run_id, reply_pkt->status);
  }
  rep_size = generic_file_size(run_report_dir, pname, "");
  if (rep_size < 0) goto failed;
  rep_flags = archive_make_write_path(state, rep_path, sizeof(rep_path),
                                      state->global->xml_report_archive_dir,
                                      reply_pkt->run_id, rep_size, 0);
  if (archive_dir_prepare(state, state->global->xml_report_archive_dir,
                          reply_pkt->run_id, 0, 0) < 0)
    goto failed;
  if (generic_copy_file(REMOVE, run_report_dir, pname, "",
                        rep_flags, 0, rep_path, "") < 0)
    goto failed;
  /*
  if (serve_state.global->team_enable_rep_view) {
    team_size = generic_file_size(run_team_report_dir, pname, "");
    team_flags = archive_make_write_path(team_path, sizeof(team_path),
                                         serve_state.global->team_report_archive_dir,
                                         reply_pkt->run_id, team_size, 0);
    if (archive_dir_prepare(serve_state.global->team_report_archive_dir,
                            reply_pkt->run_id, 0, 0) < 0)
      return -1;
    if (generic_copy_file(REMOVE, run_team_report_dir, pname, "",
                          team_flags, 0, team_path, "") < 0)
      return -1;
  }
  */
  if (state->global->enable_full_archive) {
    full_flags = archive_make_write_path(state, full_path, sizeof(full_path),
                                         state->global->full_archive_dir,
                                         reply_pkt->run_id, 0, 0);
    if (archive_dir_prepare(state, state->global->full_archive_dir,
                            reply_pkt->run_id, 0, 0) < 0)
      goto failed;
    if (generic_copy_file(REMOVE, run_full_archive_dir, pname, "",
                          0, 0, full_path, "") < 0)
      goto failed;
  }

  /* add auditing information */
  if (!(f = open_memstream(&audit_text, &audit_text_size))) return 1;
  fprintf(f, "Status: Judging complete\n");
  fprintf(f, "  Profiling information:\n");
  fprintf(f, "  Request start time:                %s\n",
          time_to_str(time_buf, sizeof(time_buf),
                      reply_pkt->ts1, reply_pkt->ts1_us));
  fprintf(f, "  Request completion time:           %s\n",
          time_to_str(time_buf, sizeof(time_buf),
                      ts8, ts8_us));
  fprintf(f, "  Total testing duration:            %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts1, reply_pkt->ts1_us,
                     ts8, ts8_us));
  fprintf(f, "  Waiting in compile queue duration: %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts1, reply_pkt->ts1_us,
                     reply_pkt->ts2, reply_pkt->ts2_us));
  fprintf(f, "  Compilation duration:              %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts2, reply_pkt->ts2_us,
                     reply_pkt->ts3, reply_pkt->ts3_us));
  fprintf(f, "  Waiting in serve queue duration:   %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts3, reply_pkt->ts3_us,
                     reply_pkt->ts4, reply_pkt->ts4_us));
  fprintf(f, "  Waiting in run queue duration:     %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts4, reply_pkt->ts4_us,
                     reply_pkt->ts5, reply_pkt->ts5_us));
  fprintf(f, "  Testing duration:                  %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts5, reply_pkt->ts5_us,
                     reply_pkt->ts6, reply_pkt->ts6_us));
  fprintf(f, "  Post-processing duration:          %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts6, reply_pkt->ts6_us,
                     reply_pkt->ts7, reply_pkt->ts7_us));
  fprintf(f, "  Waiting in serve queue duration:   %s\n",
          dur_to_str(time_buf, sizeof(time_buf),
                     reply_pkt->ts7, reply_pkt->ts7_us,
                     ts8, ts8_us));
  fprintf(f, "\n");
  close_memstream(f); f = 0;
  serve_audit_log(state, reply_pkt->run_id, 0, 0, 0, "%s", audit_text);
  xfree(audit_text); audit_text = 0;
  run_reply_packet_free(reply_pkt);

  return 1;

 bad_packet_error:
  err("bad_packet");

 failed:
  xfree(reply_buf);
  run_reply_packet_free(reply_pkt);
  return 0;
}

static const char * const scoring_system_strs[] =
{
  [SCORE_ACM] "ACM",
  [SCORE_KIROV] "KIROV",
  [SCORE_OLYMPIAD] "OLYMPIAD",
  [SCORE_MOSCOW] "MOSCOW",
};
static const unsigned char *
unparse_scoring_system(unsigned char *buf, size_t size, int val)
{
  if (val >= SCORE_ACM && val < SCORE_TOTAL) return scoring_system_strs[val];
  snprintf(buf, size, "scoring_%d", val);
  return buf;
}

void
serve_judge_built_in_problem(
        serve_state_t state,
        const struct contest_desc *cnts,
        int run_id,
        int judge_id,
        int variant,
        int accepting_mode,
        struct run_entry *re,
        const struct section_problem_data *prob,
        problem_xml_t px,
        int user_id,
        ej_ip_t ip,
        int ssl_flag)
{
  const struct section_global_data *global = state->global;
  int arch_flags, n, status, rep_flags, glob_status;
  path_t run_arch_path, rep_path;
  char *run_text = 0, *eptr = 0;
  size_t run_size = 0;
  unsigned char msgbuf[1024] = { 0 }, buf1[128], buf2[128];
  char *xml_buf = 0;
  size_t xml_len = 0;
  FILE *f = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int passed_tests = 0, score = 0, failed_test = 1;

  arch_flags = archive_make_read_path(state, run_arch_path,
                                      sizeof(run_arch_path),
                                      global->run_archive_dir, run_id,
                                      0, 0);
  if (arch_flags < 0) {
    snprintf(msgbuf, sizeof(msgbuf), "User answer file does not exist.");
    status = RUN_CHECK_FAILED;
    goto done;
  }
  if (generic_read_file(&run_text, 0, &run_size, arch_flags,
                        0, run_arch_path, 0) < 0) {
    snprintf(msgbuf, sizeof(msgbuf), "User answer file read error.");
    status = RUN_CHECK_FAILED;
    goto done;
  }
  if (strlen(run_text) != run_size) {
    snprintf(msgbuf, sizeof(msgbuf), "User answer file is binary.");
    status = RUN_PRESENTATION_ERR;
    goto done;
  }
  while (run_size > 0 && isspace(run_text[run_size - 1])) run_size--;
  run_text[run_size] = 0;

  errno = 0;
  n = strtol(run_text, &eptr, 10);
  if (*eptr || errno) {
    snprintf(msgbuf, sizeof(msgbuf), "Number expected in user answer file.");
    status = RUN_PRESENTATION_ERR;
    goto done;
  }
  if (n <= 0 || n > px->ans_num) {
    snprintf(msgbuf, sizeof(msgbuf), "Number is out of range.");
    status = RUN_PRESENTATION_ERR;
    goto done;
  }
  if (n != px->correct_answer) {
    snprintf(msgbuf, sizeof(msgbuf), "Wrong answer.");
    status = RUN_WRONG_ANSWER_ERR;
    goto done;
  }

  passed_tests = 1;
  failed_test = 0;
  score = prob->full_score;
  status = RUN_OK;

 done:
  glob_status = status;
  if (global->score_system == SCORE_OLYMPIAD
      || global->score_system == SCORE_KIROV) {
    if (glob_status != RUN_OK && glob_status != RUN_CHECK_FAILED)
      glob_status = RUN_PARTIAL;
  }
  f = open_memstream(&xml_buf, &xml_len);
  fprintf(f, "Content-type: text/xml\n\n");
  fprintf(f, "<?xml version=\"1.0\" encoding=\"%s\"?>\n", EJUDGE_CHARSET);
  run_status_to_str_short(buf1, sizeof(buf1), glob_status);
  fprintf(f, "<testing-report run-id=\"%d\" judge-id=\"%d\" status=\"%s\" scoring=\"%s\" archive-available=\"no\" run-tests=\"1\" correct-available=\"no\" info-available=\"no\"",
          run_id, judge_id, buf1,
          unparse_scoring_system(buf2, sizeof(buf2), global->score_system));
  if (variant > 0) {
    fprintf(f, " variant=\"%d\"", variant);
  }
  if (global->score_system == SCORE_OLYMPIAD) {
    fprintf(f, " accepting-mode=\"%s\"", accepting_mode?"yes":"no");
  }
  if (global->score_system == SCORE_OLYMPIAD && accepting_mode
      && status != RUN_ACCEPTED) {
    fprintf(f, " failed-test=\"1\"");
  } else if (global->score_system == SCORE_ACM && status != RUN_OK) {
    fprintf(f, " failed-test=\"1\"");
  } else if (global->score_system == SCORE_OLYMPIAD && !accepting_mode) {
    fprintf(f, " tests-passed=\"%d\" score=\"%d\" max-score=\"%d\"",
            passed_tests, score, prob->full_score);
  } else if (global->score_system == SCORE_KIROV) {
    fprintf(f, " tests-passed=\"%d\" score=\"%d\" max-score=\"%d\"",
            passed_tests, score, prob->full_score);
  } else if (global->score_system == SCORE_MOSCOW) {
    if (status != RUN_OK) {
      fprintf(f, " failed-test=\"1\"");
    }
    fprintf(f, " score=\"%d\" max-score=\"%d\"", score, prob->full_score);
  }
  fprintf(f, " >\n");

  run_status_to_str_short(buf1, sizeof(buf1), status);
  fprintf(f, "  <tests>\n");
  fprintf(f, "    <test num=\"1\" status=\"%s\"", buf1);
  fprintf(f, " exit-code=\"0\"");
  fprintf(f, " time=\"0\"");
  fprintf(f, " real-time=\"0\"");
  if (global->score_system == SCORE_OLYMPIAD && !accepting_mode) {
    fprintf(f, " nominal-score=\"%d\" score=\"%d\"", prob->full_score, score);
  } else if (global->score_system == SCORE_KIROV) {
    fprintf(f, " nominal-score=\"%d\" score=\"%d\"", prob->full_score, score);
  }
  if (msgbuf[0]) {
    fprintf(f, " checker-comment=\"%s\"", ARMOR(msgbuf));
  }
  fprintf(f, " >\n");

  /*
    if (tests[i].input_size >= 0 && !req_pkt->full_archive) {
      fprintf(f, "      <input>");
      html_print_by_line(f, tests[i].input, tests[i].input_size);
      fprintf(f, "</input>\n");
    }
  */

  fprintf(f, "      <output>%s\n\n</output>\n", ARMOR(run_text));
  fprintf(f, "      <correct>%d\n\n</correct>\n", px->correct_answer);
  fprintf(f, "      <checker>%s\n\n</checker>\n", ARMOR(msgbuf));
  fprintf(f, "    </test>\n");
  fprintf(f, "  </tests>\n");
  fprintf(f, "</testing-report>\n");
  close_memstream(f); f = 0;

  serve_audit_log(state, run_id, user_id, ip, ssl_flag,
                  "Command: submit\n"
                  "Status: ok\n"
                  "Run-id: %d\n", run_id);

  if (status == RUN_CHECK_FAILED)
    serve_send_check_failed_email(cnts, run_id);

  /* FIXME: handle database update error */
  run_change_status(state->runlog_state, run_id, glob_status, failed_test,
                    score, 0);
  serve_update_standings_file(state, cnts, 0);
  /*
  if (global->notify_status_change > 0 && !re.is_hidden
      && comp_extra->notify_flag) {
    serve_notify_user_run_status_change(cnts, state, re.user_id,
                                        run_id, glob_status);
  }
  */
  rep_flags = archive_make_write_path(state, rep_path, sizeof(rep_path),
                                      global->xml_report_archive_dir,
                                      run_id, xml_len, 0);
  archive_dir_prepare(state, global->xml_report_archive_dir, run_id, 0, 0);
  generic_write_file(xml_buf, xml_len, rep_flags, 0, rep_path, "");
  serve_audit_log(state, run_id, 0, 0, 0,
                  "Status: judging complete (built-in)\n");

  xfree(xml_buf); xml_buf = 0;
  html_armor_free(&ab);
}

void
serve_rejudge_run(
        const struct contest_desc *cnts,
        serve_state_t state,
        int run_id,
        int user_id,
        ej_ip_t ip,
        int ssl_flag,
        int force_full_rejudge,
        int priority_adjustment)
{
  struct run_entry re;
  int accepting_mode = -1, arch_flags = 0;
  path_t run_arch_path;
  char *run_text = 0;
  size_t run_size = 0;
  const struct section_problem_data *prob = 0;
  const struct section_language_data *lang = 0;
  problem_xml_t px = 0;
  int variant = 0;

  if (run_get_entry(state->runlog_state, run_id, &re) < 0) return;
  if (re.is_imported) return;
  if (re.is_readonly) return;
 
  if (re.prob_id <= 0 || re.prob_id > state->max_prob
      || !(prob = state->probs[re.prob_id])) {
    err("rejudge_run: bad problem: %d", re.prob_id);
    return;
  }
  if (prob->manual_checking > 0 || prob->disable_testing > 0) return;
  if (prob->type > 0) {
    if (force_full_rejudge
        && state->global->score_system == SCORE_OLYMPIAD) {
      accepting_mode = 0;
    }

    if (prob->variant_num > 0) {
      if (variant <= 0)
        variant = re.variant;
      if (variant <= 0)
        variant = find_variant(state, re.user_id, re.prob_id, 0);
      if (variant <= 0 || variant > prob->variant_num) {
        err("rejudge_run: invalid variant for run %d", run_id);
        return;
      }
      if (prob->xml.a) px = prob->xml.a[variant - 1];
    } else {
      px = prob->xml.p;
    }

    if (prob->type == PROB_TYPE_SELECT_ONE && px && px->ans_num > 0) {
      serve_judge_built_in_problem(state, cnts, run_id, 1 /* judge_id*/,
                                   variant, accepting_mode, &re, prob,
                                   px, user_id, ip, ssl_flag);
      return;
    }

    arch_flags = archive_make_read_path(state, run_arch_path,
                                        sizeof(run_arch_path),
                                        state->global->run_archive_dir, run_id,
                                        0, 0);
    if (arch_flags < 0) return;
    if (generic_read_file(&run_text, 0, &run_size, arch_flags,
                          0, run_arch_path, 0) < 0)
      return;

    serve_run_request(state, stderr, run_text, run_size, run_id,
                      re.user_id, re.prob_id, 0, 0, priority_adjustment,
                      -1, accepting_mode, 1, 0, 0);
    xfree(run_text);

    serve_audit_log(state, run_id, user_id, ip, ssl_flag, "Command: Rejudge\n");
    return;
  }

  if (re.lang_id <= 0 || re.lang_id > state->max_lang
      || !(lang = state->langs[re.lang_id])) {
    err("rejudge_run: bad language: %d", re.lang_id);
    return;
  }

  if (force_full_rejudge && state->global->score_system == SCORE_OLYMPIAD) {
    accepting_mode = 0;
  }

  serve_compile_request(state, 0, -1, run_id, re.user_id,
                        state->langs[re.lang_id]->compile_id, re.locale_id,
                        (prob->type > 0),
                        state->langs[re.lang_id]->src_sfx,
                        state->langs[re.lang_id]->compiler_env,
                        accepting_mode, priority_adjustment, 1, prob, lang);

  serve_audit_log(state, run_id, user_id, ip, ssl_flag, "Command: Rejudge\n");
}

void
serve_invoke_start_script(serve_state_t state)
{
  tpTask tsk = 0;

  if (!state->global->contest_start_cmd[0]) return;
  if (!(tsk = task_New())) return;
  task_AddArg(tsk, state->global->contest_start_cmd);
  task_SetPathAsArg0(tsk);
  if (task_Start(tsk) < 0) {
    task_Delete(tsk);
    return;
  }
  task_Wait(tsk);
  task_Delete(tsk);
}

void
serve_send_run_quit(const serve_state_t state)
{
  void *pkt_buf = 0;
  size_t pkt_size = 0;

  run_request_packet_quit(&pkt_size, &pkt_buf);
  generic_write_file(pkt_buf, pkt_size, SAFE, state->global->run_queue_dir,
                     "QUIT", "");
  xfree(pkt_buf);
}

static unsigned char olympiad_rejudgeable_runs[RUN_LAST + 1] =
{
  [RUN_OK]               = 1,
  [RUN_COMPILE_ERR]      = 0,
  [RUN_RUN_TIME_ERR]     = 0,
  [RUN_TIME_LIMIT_ERR]   = 0,
  [RUN_PRESENTATION_ERR] = 0,
  [RUN_WRONG_ANSWER_ERR] = 0,
  [RUN_CHECK_FAILED]     = 0,
  [RUN_PARTIAL]          = 1,
  [RUN_ACCEPTED]         = 1,
  [RUN_IGNORED]          = 0,
  [RUN_DISQUALIFIED]     = 0,
  [RUN_PENDING]          = 0,
  [RUN_MEM_LIMIT_ERR]    = 0,
  [RUN_SECURITY_ERR]     = 0,
  [RUN_STYLE_ERR]        = 0,
  [RUN_VIRTUAL_START]    = 0,
  [RUN_VIRTUAL_STOP]     = 0,
  [RUN_EMPTY]            = 0,
  [RUN_RUNNING]          = 1,
  [RUN_COMPILED]         = 1,
  [RUN_COMPILING]        = 1,
  [RUN_AVAILABLE]        = 1,
};

static unsigned char olympiad_output_only_rejudgeable_runs[RUN_LAST + 1] =
{
  [RUN_OK]               = 1,
  [RUN_COMPILE_ERR]      = 0,
  [RUN_RUN_TIME_ERR]     = 0,
  [RUN_TIME_LIMIT_ERR]   = 0,
  [RUN_PRESENTATION_ERR] = 0,
  [RUN_WRONG_ANSWER_ERR] = 1,
  [RUN_CHECK_FAILED]     = 0,
  [RUN_PARTIAL]          = 1,
  [RUN_ACCEPTED]         = 1,
  [RUN_IGNORED]          = 0,
  [RUN_DISQUALIFIED]     = 0,
  [RUN_PENDING]          = 0,
  [RUN_MEM_LIMIT_ERR]    = 0,
  [RUN_SECURITY_ERR]     = 0,
  [RUN_STYLE_ERR]        = 0,
  [RUN_VIRTUAL_START]    = 0,
  [RUN_VIRTUAL_STOP]     = 0,
  [RUN_EMPTY]            = 0,
  [RUN_RUNNING]          = 1,
  [RUN_COMPILED]         = 1,
  [RUN_COMPILING]        = 1,
  [RUN_AVAILABLE]        = 1,
};

static unsigned char generally_rejudgable_runs[RUN_LAST + 1] =
{
  [RUN_OK]               = 1,
  [RUN_COMPILE_ERR]      = 1,
  [RUN_RUN_TIME_ERR]     = 1,
  [RUN_TIME_LIMIT_ERR]   = 1,
  [RUN_PRESENTATION_ERR] = 1,
  [RUN_WRONG_ANSWER_ERR] = 1,
  [RUN_CHECK_FAILED]     = 1,
  [RUN_PARTIAL]          = 1,
  [RUN_ACCEPTED]         = 1,
  [RUN_IGNORED]          = 1,
  [RUN_DISQUALIFIED]     = 1,
  [RUN_PENDING]          = 1,
  [RUN_MEM_LIMIT_ERR]    = 1,
  [RUN_SECURITY_ERR]     = 1,
  [RUN_STYLE_ERR]        = 1,
  [RUN_VIRTUAL_START]    = 0,
  [RUN_VIRTUAL_STOP]     = 0,
  [RUN_EMPTY]            = 0,
  [RUN_RUNNING]          = 1,
  [RUN_COMPILED]         = 1,
  [RUN_COMPILING]        = 1,
  [RUN_AVAILABLE]        = 1,
};

static int
is_generally_rejudgable(const serve_state_t state,
                        const struct run_entry *pe,
                        int total_users)
{
  const struct section_problem_data *prob = 0;
  const struct section_language_data *lang = 0;

  if (pe->status > RUN_LAST) return 0;
  if (!generally_rejudgable_runs[pe->status]) return 0;
  if (pe->is_imported) return 0;
  if (pe->is_readonly) return 0;
  if (pe->user_id <= 0 || pe->user_id >= total_users) return 0;
  if (pe->prob_id <= 0 || pe->prob_id > state->max_prob
      || !(prob = state->probs[pe->prob_id])) return 0;
  if (prob->disable_testing) return 0;
  if (prob->type == PROB_TYPE_STANDARD) {
    if (pe->lang_id <= 0 || pe->lang_id > state->max_lang
        || !(lang = state->langs[pe->lang_id])) return 0;
    if (lang->disable_testing) return 0;
  }
  if (prob->manual_checking) return 0;

  return 1;
}

#define BITS_PER_LONG (8*sizeof(unsigned long)) 

/* Since we're provided the exact set of runs to rejudge, we ignore
 * "latest" condition in OLYMPIAD contests, or DISQUALIFIED or IGNORED
 * runs
 */
void
serve_rejudge_by_mask(
        const struct contest_desc *cnts,
        serve_state_t state,
        int user_id,
        ej_ip_t ip,
        int ssl_flag,
        int mask_size,
        unsigned long *mask,
        int force_flag,
        int priority_adjustment)
{
  int total_runs, r;
  struct run_entry re;

  ASSERT(mask_size > 0);

  total_runs = run_get_total(state->runlog_state);
  if (total_runs > mask_size * BITS_PER_LONG) {
    total_runs = mask_size * BITS_PER_LONG;
  }

  /*
  if (state->global->score_system == SCORE_OLYMPIAD
      && !state->accepting_mode) {
    // rejudge only "ACCEPTED", "OK", "PARTIAL SOLUTION" runs,
    // considering only the last run for the given problem and
    // the given participant
    total_ids = teamdb_get_max_team_id(state->teamdb_state) + 1;
    total_probs = state->max_prob + 1;
    size = total_ids * total_probs;

    if (total_ids <= 0 || total_probs <= 0) return;
    flag = (unsigned char *) alloca(size);
    memset(flag, 0, size);
    for (r = total_runs - 1; r >= 0; r--) {
      if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
      if (!run_is_source_available(re.status)) continue;
      if (re.is_imported) continue;
      if (re.is_readonly) continue;

      if (re.status != RUN_OK && re.status != RUN_PARTIAL
          && re.status != RUN_ACCEPTED) continue;
      if (re.user_id <= 0 || re.user_id >= total_ids) continue;
      if (re.prob_id <= 0 || re.prob_id >= total_probs) continue;
      if (re.lang_id <= 0 || re.lang_id > state->max_lang) continue;
      if (!state->probs[re.prob_id]
          || state->probs[re.prob_id]->disable_testing) continue;
      if (!state->langs[re.lang_id]
          || state->langs[re.lang_id]->disable_testing) continue;
      if (!(mask[r / BITS_PER_LONG] & (1L << (r % BITS_PER_LONG)))) continue;
      idx = re.user_id * total_probs + re.prob_id;
      if (flag[idx]) continue;
      flag[idx] = 1;
      serve_rejudge_run(state, r, user_id, ip, ssl_flag, 0, 0);
    }
    return;
  }
  */

  for (r = 0; r < total_runs; r++) {
    if (run_get_entry(state->runlog_state, r, &re) >= 0
        && is_generally_rejudgable(state, &re, INT_MAX)
        && (mask[r / BITS_PER_LONG] & (1L << (r % BITS_PER_LONG)))) {
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag,
                        force_flag, priority_adjustment);
    }
  }
}

void
serve_rejudge_problem(
        const struct contest_desc *cnts,
        serve_state_t state,
        int user_id,
        ej_ip_t ip,
        int ssl_flag,
        int prob_id)
{
  int total_runs, r;
  struct run_entry re;
  int total_ids;
  unsigned char *flag;

  if (prob_id <= 0 || prob_id > state->max_prob || !state->probs[prob_id]
      || state->probs[prob_id]->disable_testing) return;
  total_runs = run_get_total(state->runlog_state);

  if (state->global->score_system == SCORE_OLYMPIAD
      && !state->accepting_mode) {
    // rejudge only "ACCEPTED", "OK", "PARTIAL SOLUTION" runs,
    // considering only the last run for the given participant
    total_ids = teamdb_get_max_team_id(state->teamdb_state) + 1;

    if (total_ids <= 0) return;
    flag = (unsigned char *) alloca(total_ids);
    memset(flag, 0, total_ids);
    for (r = total_runs - 1; r >= 0; r--) {
      if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
      if (!is_generally_rejudgable(state, &re, total_ids)) continue;
      if (state->probs[re.prob_id]->type != PROB_TYPE_STANDARD) {
        if (!olympiad_output_only_rejudgeable_runs[re.status]) continue;
      } else {
        if (!olympiad_rejudgeable_runs[re.status]) continue;
      }
      if (re.prob_id != prob_id) continue;
      if (flag[re.user_id]) continue;
      flag[re.user_id] = 1;
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag, 0, 0);
    }
    return;
  }

  for (r = 0; r < total_runs; r++) {
    if (run_get_entry(state->runlog_state, r, &re) >= 0
        && is_generally_rejudgable(state, &re, INT_MAX)
        && re.status != RUN_IGNORED && re.status != RUN_DISQUALIFIED
        && re.prob_id == prob_id) {
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag, 0, 0);
    }
  }
}

void
serve_judge_suspended(
        const struct contest_desc *cnts,
        serve_state_t state,
        int user_id,
        ej_ip_t ip,
        int ssl_flag)
{
  int total_runs, r;
  struct run_entry re;

  total_runs = run_get_total(state->runlog_state);

  if (state->global->score_system == SCORE_OLYMPIAD
      && !state->accepting_mode)
    return;

  for (r = 0; r < total_runs; r++) {
    if (run_get_entry(state->runlog_state, r, &re) >= 0
        && is_generally_rejudgable(state, &re, INT_MAX)
        && re.status == RUN_PENDING) {
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag, 0, 0);
    }
  }
}

void
serve_rejudge_all(
        const struct contest_desc *cnts,
        serve_state_t state,
        int user_id,
        ej_ip_t ip,
        int ssl_flag)
{
  int total_runs, r, size, idx, total_ids, total_probs;
  struct run_entry re;
  unsigned char *flag;

  total_runs = run_get_total(state->runlog_state);

  if (state->global->score_system == SCORE_OLYMPIAD
      && !state->accepting_mode) {
    // rejudge only "ACCEPTED", "OK", "PARTIAL SOLUTION" runs,
    // considering only the last run for the given problem and
    // the given participant
    total_ids = teamdb_get_max_team_id(state->teamdb_state) + 1;
    total_probs = state->max_prob + 1;
    size = total_ids * total_probs;

    if (total_ids <= 0 || total_probs <= 0) return;
    flag = (unsigned char *) alloca(size);
    memset(flag, 0, size);
    for (r = total_runs - 1; r >= 0; r--) {
      if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
      if (!is_generally_rejudgable(state, &re, total_ids)) continue;
      if (state->probs[re.prob_id]->type != PROB_TYPE_STANDARD) {
        if (!olympiad_output_only_rejudgeable_runs[re.status]) continue;
      } else {
        if (!olympiad_rejudgeable_runs[re.status]) continue;
      }
      idx = re.user_id * total_probs + re.prob_id;
      if (flag[idx]) continue;
      flag[idx] = 1;
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag, 0, 0);
    }
    return;
  }

  for (r = 0; r < total_runs; r++) {
    if (run_get_entry(state->runlog_state, r, &re) >= 0
        && is_generally_rejudgable(state, &re, INT_MAX)
        && re.status != RUN_IGNORED && re.status != RUN_DISQUALIFIED) {
      serve_rejudge_run(cnts, state, r, user_id, ip, ssl_flag, 0, 0);
    }
  }
}

void
serve_reset_contest(const struct contest_desc *cnts, serve_state_t state)
{
  run_reset(state->runlog_state, state->global->contest_time,
            cnts->sched_time, state->global->contest_finish_time);
  run_set_duration(state->runlog_state,
                   state->global->contest_time);
  clar_reset(state->clarlog_state);

  /* clear all submissions and clarifications */
  if (state->global->xml_report_archive_dir[0])
    clear_directory(state->global->xml_report_archive_dir);
  if (state->global->report_archive_dir[0])
    clear_directory(state->global->report_archive_dir);
  if (state->global->run_archive_dir[0])
    clear_directory(state->global->run_archive_dir);
  if (state->global->team_report_archive_dir[0])
    clear_directory(state->global->team_report_archive_dir);
  if (state->global->full_archive_dir[0])
    clear_directory(state->global->full_archive_dir);
  if (state->global->audit_log_dir[0])
    clear_directory(state->global->audit_log_dir);
  if (state->global->team_extra_dir[0])
    clear_directory(state->global->team_extra_dir);
}

void
serve_squeeze_runs(serve_state_t state)
{
  int i, j, tot;

  // sorry...
  return;

  tot = run_get_total(state->runlog_state);
  for (i = 0, j = 0; i < tot; i++) {
    if (run_get_status(state->runlog_state, i) == RUN_EMPTY) continue;
    if (i != j) {
      archive_rename(state, state->global->run_archive_dir, 0, i, 0, j, 0, 0);
      archive_rename(state, state->global->xml_report_archive_dir, 0, i, 0, j, 0, 1);
      archive_rename(state, state->global->report_archive_dir, 0, i, 0, j, 0, 1);
      if (state->global->team_enable_rep_view) {
        archive_rename(state, state->global->team_report_archive_dir, 0, i, 0, j, 0, 0);
      }
      if (state->global->enable_full_archive) {
        archive_rename(state, state->global->full_archive_dir, 0, i, 0, j, 0, 0);
      }
      archive_rename(state, state->global->audit_log_dir, 0, i, 0, j, 0, 1);
    }
    j++;
  }
  for (; j < tot; j++) {
    archive_remove(state, state->global->run_archive_dir, j, 0);
    archive_remove(state, state->global->xml_report_archive_dir, j, 0);
    archive_remove(state, state->global->report_archive_dir, j, 0);
    if (state->global->team_enable_rep_view) {
      archive_remove(state, state->global->team_report_archive_dir, j, 0);
    }
    if (state->global->enable_full_archive) {
      archive_remove(state, state->global->full_archive_dir, j, 0);
    }
    archive_remove(state, state->global->audit_log_dir, j, 0);
  }
  run_squeeze_log(state->runlog_state);

  /* FIXME: add an audit record for each renumbered run */
}

int
serve_count_transient_runs(serve_state_t state)
{
  int total_runs, r, counter = 0;
  struct run_entry re;

  total_runs = run_get_total(state->runlog_state);
  for (r = 0; r < total_runs; r++) {
    if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
    if (re.status >= RUN_TRANSIENT_FIRST && re.status <= RUN_TRANSIENT_LAST)
      counter++;
  }
  return counter;
}

#if 0
static int
check_file(const serve_state_t cs, const unsigned char *base_dir, int serial)
{
  path_t pp;

  return archive_make_read_path(cs, pp, sizeof(pp), base_dir, serial, 0, 1);
}
#endif

int
serve_collect_virtual_stop_events(serve_state_t cs)
{
  //const struct section_global_data *global = cs->global;
  struct run_header head;
  const struct run_entry *runs, *pe;
  int total_runs, i;
  time_t *user_time = 0, *new_time, *pt;
  int user_time_size = 0, new_size;
  int need_reload = 0;

  if (!cs->global->is_virtual) return 0;

  run_get_header(cs->runlog_state, &head);
  if (!head.duration) return 0;
  total_runs = run_get_total(cs->runlog_state);
  runs = run_get_entries_ptr(cs->runlog_state);

  user_time_size = 128;
  XCALLOC(user_time, user_time_size);

#if 0
  for (i = 0; i < total_runs; i++) {
    if (!run_is_valid_status(runs[i].status)) continue;
    if (runs[i].status >= RUN_PSEUDO_FIRST
        && runs[i].status <= RUN_PSEUDO_LAST) continue;
    if (runs[i].is_imported) continue;

    check_file(cs, global->run_archive_dir, i);
    if (runs[i].status >= RUN_TRANSIENT_FIRST) continue;
    if (runs[i].status == RUN_IGNORED || runs[i].status == RUN_DISQUALIFIED || runs[i].status ==RUN_PENDING) continue;
    if (check_file(cs, global->xml_report_archive_dir, i) < 0) {
      check_file(cs, global->report_archive_dir, i);
      if (global->team_enable_rep_view)
        check_file(cs, global->team_report_archive_dir, i);
    }
    if (global->enable_full_archive)
      check_file(cs, global->full_archive_dir, i);
    //check_file(cs, global->audit_log_dir, i);
  }
#endif

  for (i = 0; i < total_runs; i++) {
    pe = &runs[i];
    if (!run_is_valid_status(pe->status)) continue;
    if (pe->status == RUN_EMPTY) continue;
    if (pe->user_id <= 0 || pe->user_id >= 100000) continue;
    if (pe->user_id >= user_time_size) {
      new_size = user_time_size;
      while (pe->user_id >= new_size) new_size *= 2;
      XCALLOC(new_time, new_size);
      memcpy(new_time, user_time, user_time_size * sizeof(user_time[0]));
      xfree(user_time);
      user_time = new_time;
      user_time_size = new_size;
    }
    pt = &user_time[pe->user_id];
    if (pe->status == RUN_VIRTUAL_START) {
      if (*pt == -2) {
        err("run %d: virtual start after non-virtual runs, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else if (*pt == -1) {
        err("run %d: virtual start after virtual stop, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else if (*pt > 0) {
        err("run %d: virtual start after virtual start, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else {
        *pt = pe->time;
      }
    } else if (pe->status == RUN_VIRTUAL_STOP) {
      if (*pt == -2) {
        err("run %d: virtual stop after non-virtual runs, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else if (*pt == -1) {
        err("run %d: virtual stop after virtual stop, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else if (!*pt) {
        err("run %d: virtual stop without virtual start, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else if (pe->time > *pt + head.duration) {
        err("run %d: virtual stop time overrun, cleared!", i);
        run_forced_clear_entry(cs->runlog_state, i);
        need_reload = 1;
      } else {
        *pt = -1;
      }
    } else {
      if (*pt == -2) {
        // another non-virtual run
      } else if (*pt == -1) {
        // run after virtual stop
        if (!pe->is_hidden) {
          err("run %d: run after virtual stop, made hidden!", i);
          run_set_hidden(cs->runlog_state, i);
          need_reload = 1;
        }
      } else if (!*pt) {
        // first run
        *pt = -2;
      } else if (pe->time > *pt + head.duration) {
        // virtual run overrun
        if (!pe->is_hidden) {
          err("run %d: virtual time run overrun, made hidden!", i);
          run_set_hidden(cs->runlog_state, i);
          need_reload = 1;
        }
      } else {
        // regular virtual run
      }
    }
  }

  if (need_reload) {
    xfree(user_time); user_time = 0;
    return 1;
  }

  for (i = 1; i < user_time_size; i++)
    if (user_time[i] > 0) {
      serve_event_add(cs, user_time[i] + head.duration,
                      SERVE_EVENT_VIRTUAL_STOP, i, 0);
    }

  xfree(user_time); user_time = 0;
  return 0;
}

static void
handle_virtual_stop_event(
        const struct contest_desc *cnts,
        serve_state_t cs,
        struct serve_event_queue *p)
{
  int trans_runs = -1, nsec = -1, run_id;
  struct timeval precise_time;

  /* FIXME: if we're trying to add backlogged virtual stop while
   * having transient runs, we do some kind of busy wait
   * by not removing the pending virtual stop event from the
   * event queue...
   */

  if (p->time < cs->current_time) {
    if (trans_runs < 0) trans_runs = serve_count_transient_runs(cs);
    if (trans_runs > 0) return;
    info("inserting backlogged virtual stop event at time %ld", p->time);
    nsec = -1;
  } else {
    // p->time == cs->current_time
    gettimeofday(&precise_time, 0);
    if (precise_time.tv_sec != cs->current_time) {
      // oops...
      info("inserting virtual stop event 1 second past");
      nsec = -1;
    } else {
      info("inserting virtual stop event");
      nsec = precise_time.tv_usec * 1000;
    }
  }

  run_id = run_virtual_stop(cs->runlog_state, p->user_id, p->time,
                            0 /* IP */, 0, nsec);
  if (run_id < 0) {
    err("insert failed, removing event!");
    serve_event_remove(cs, p);
    return;
  }
  info("inserted virtual stop as run %d", run_id);
  serve_move_files_to_insert_run(cs, run_id);
  if (cs->global->score_system == SCORE_OLYMPIAD
      && cs->global->is_virtual && cs->global->disable_virtual_auto_judge<= 0) {
    serve_event_add(cs, p->time + 1, SERVE_EVENT_JUDGE_OLYMPIAD, p->user_id, 0);
  }
  if (p->handler) (*p->handler)(cnts, cs, p);
  serve_event_remove(cs, p);
}

static void
handle_judge_olympiad_event(
        const struct contest_desc *cnts,
        serve_state_t cs,
        struct serve_event_queue *p)
{
  int count;
  struct run_entry rs, re;

  if (cs->global->score_system != SCORE_OLYMPIAD
      || !cs->global->is_virtual) goto done;
  count = run_get_virtual_info(cs->runlog_state, p->user_id, &rs, &re);
  if (count < 0) {
    err("virtual user %d cannot be judged", p->user_id);
    goto done;
  }
  // cannot do judging before all transint runs are done
  if (count > 0) return;
  if (rs.status != RUN_VIRTUAL_START || rs.user_id != p->user_id)
    goto done;
  if (re.status != RUN_VIRTUAL_STOP || re.user_id != p->user_id)
    goto done;
  // already judged somehow
  if (rs.judge_id > 0) goto done;
  serve_judge_virtual_olympiad(cnts, cs, p->user_id, re.run_id);
  if (p->handler) (*p->handler)(cnts, cs, p);

 done:
  serve_event_remove(cs, p);
  return;
}

void
serve_handle_events(const struct contest_desc *cnts, serve_state_t cs)
{
  struct serve_event_queue *p, *q;

  if (!cs->event_first) return;

  for (p = cs->event_first; p; p = q) {
    q = p->next;
    if (p->time > cs->current_time) break;
    switch (p->type) {
    case SERVE_EVENT_VIRTUAL_STOP:
      handle_virtual_stop_event(cnts, cs, p);
      break;
    case SERVE_EVENT_JUDGE_OLYMPIAD:
      handle_judge_olympiad_event(cnts, cs, p);
      break;
    default:
      abort();
    }
  }
}

void
serve_judge_virtual_olympiad(
        const struct contest_desc *cnts,
        serve_state_t cs,
        int user_id,
        int run_id)
{
  const struct section_global_data *global = cs->global;
  const struct section_problem_data *prob;
  struct run_entry re;
  int *latest_runs, s, i;
  int vstart_id;

  if (global->score_system != SCORE_OLYMPIAD || !global->is_virtual) return;
  if (user_id <= 0) return;
  if (run_get_virtual_start_entry(cs->runlog_state, user_id, &re) < 0) return;
  if (re.judge_id > 0) return;
  if (run_id < 0) return;
  vstart_id = re.run_id;

  // Fully rejudge latest submits
  if (run_get_entry(cs->runlog_state, run_id, &re) < 0) return;
  if (re.status != RUN_VIRTUAL_STOP) return;
  if (cs->max_prob <= 0) return;

  XALLOCA(latest_runs, cs->max_prob + 1);
  memset(latest_runs, -1, (cs->max_prob + 1) * sizeof(latest_runs[0]));
  run_id--;
  for (;run_id >= 0; run_id--) {
    if (run_get_entry(cs->runlog_state, run_id, &re) < 0) return;
    if (!run_is_valid_status((s = re.status))) continue;
    if (s == RUN_EMPTY) continue;
    if (re.user_id != user_id) continue;
    if (s == RUN_VIRTUAL_START) break;
    prob = 0;
    if (re.prob_id > 0 && re.prob_id <= cs->max_prob)
      prob = cs->probs[re.prob_id];
    if (!prob) continue;
    if (prob->disable_testing || prob->disable_auto_testing) continue;
    if (s != RUN_OK && s != RUN_PARTIAL && s != RUN_ACCEPTED
        && (s != RUN_WRONG_ANSWER_ERR || prob->type == PROB_TYPE_STANDARD))
        continue;
    if (latest_runs[re.prob_id] < 0) latest_runs[re.prob_id] = run_id;
  }
  if (run_id < 0) return;

  for (i = 1; i <= cs->max_prob; i++) {
    if (latest_runs[i] >= 0)
      serve_rejudge_run(cnts, cs, latest_runs[i], user_id, 0, 0, 1, 10);
  }
  run_set_judge_id(cs->runlog_state, vstart_id, 1);
}

void
serve_clear_by_mask(serve_state_t state,
                    int user_id, ej_ip_t ip, int ssl_flag,
                    int mask_size, unsigned long *mask)
{
  int total_runs, r;
  const struct section_global_data *global = state->global;

  ASSERT(mask_size > 0);

  total_runs = run_get_total(state->runlog_state);
  if (total_runs > mask_size * BITS_PER_LONG) {
    total_runs = mask_size * BITS_PER_LONG;
  }

  for (r = total_runs - 1; r >= 0; r--) {
    if ((mask[r / BITS_PER_LONG] & (1L << (r % BITS_PER_LONG)))
        && !run_is_readonly(state->runlog_state, r)) {
      if (run_clear_entry(state->runlog_state, r) >= 0) {
        archive_remove(state, global->run_archive_dir, r, 0);
        archive_remove(state, global->xml_report_archive_dir, r, 0);
        archive_remove(state, global->report_archive_dir, r, 0);
        if (global->team_enable_rep_view) {
          archive_remove(state, global->team_report_archive_dir, r, 0);
        }
        if (global->enable_full_archive) {
          archive_remove(state, global->full_archive_dir, r, 0);
        }
        archive_remove(state, global->audit_log_dir, r, 0);
      }
    }
  }
}

void
serve_ignore_by_mask(serve_state_t state,
                     int user_id, ej_ip_t ip, int ssl_flag,
                     int mask_size, unsigned long *mask,
                     int new_status)
{
  int total_runs, r;
  struct run_entry re;
  const unsigned char *cmd = 0;
  const struct section_global_data *global = state->global;

  ASSERT(mask_size > 0);

  switch (new_status) {
  case RUN_IGNORED:
    cmd = "Ignore";
    break;
  case RUN_DISQUALIFIED:
    cmd = "Disqualify";
    break;
  default:
    abort();
  }

  total_runs = run_get_total(state->runlog_state);
  if (total_runs > mask_size * BITS_PER_LONG) {
    total_runs = mask_size * BITS_PER_LONG;
  }

  for (r = total_runs - 1; r >= 0; r--) {
    if (!(mask[r / BITS_PER_LONG] & (1L << (r % BITS_PER_LONG)))
        || run_is_readonly(state->runlog_state, r))
      continue;
    if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
    if (!run_is_valid_status(re.status)) continue;
    // do not change EMPTY, VIRTUAL_START, VIRTUAL_STOP runs
    if (re.status > RUN_MAX_STATUS && re.status < RUN_TRANSIENT_FIRST)
      continue;
    if (re.status == new_status) continue;

    re.status = new_status;
    if (run_set_entry(state->runlog_state, r, RE_STATUS, &re) >= 0) {
      archive_remove(state, global->xml_report_archive_dir, r, 0);
      archive_remove(state, global->report_archive_dir, r, 0);
      if (global->team_enable_rep_view) {
        archive_remove(state, global->team_report_archive_dir, r, 0);
      }
      if (global->enable_full_archive) {
        archive_remove(state, global->full_archive_dir, r, 0);
      }
      serve_audit_log(state, r, user_id, ip, ssl_flag, "Command: %s\n", cmd);
    }
  }
}

void
serve_mark_by_mask(
        serve_state_t state,
        int user_id,
        ej_ip_t ip,
        int ssl_flag,
        int mask_size,
        unsigned long *mask,
        int mark_value)
{
  int total_runs, r;
  struct run_entry re;

  ASSERT(mask_size > 0);
  mark_value = !!mark_value;

  total_runs = run_get_total(state->runlog_state);
  if (total_runs > mask_size * BITS_PER_LONG) {
    total_runs = mask_size * BITS_PER_LONG;
  }

  for (r = total_runs - 1; r >= 0; r--) {
    if (!(mask[r / BITS_PER_LONG] & (1L << (r % BITS_PER_LONG)))
        || run_is_readonly(state->runlog_state, r))
      continue;
    if (run_get_entry(state->runlog_state, r, &re) < 0) continue;
    if (!run_is_valid_status(re.status)) continue;
    if (re.status > RUN_MAX_STATUS) continue;
    if (re.is_marked == mark_value) continue;

    re.is_marked = mark_value;
    run_set_entry(state->runlog_state, r, RE_IS_MARKED, &re);
  }
}

static int
testing_queue_unlock_entry(
        const unsigned char *run_queue_dir,
        const unsigned char *out_path,
        const unsigned char *packet_name);

static int
testing_queue_lock_entry(
        int contest_id,
        const unsigned char *run_queue_dir,
        const unsigned char *packet_name,
        unsigned char *out_name,
        size_t out_size,
        unsigned char *out_path,
        size_t out_path_size,
        struct run_request_packet **packet)
{
  path_t dir_path;
  struct stat sb;
  char *pkt_buf = 0;
  size_t pkt_size = 0;

  snprintf(out_name, out_size, "%s_%d_%s",
           os_NodeName(), getpid(), packet_name);
  snprintf(dir_path, sizeof(dir_path), "%s/dir/%s",
           run_queue_dir, packet_name);
  snprintf(out_path, out_path_size, "%s/out/%s",
           run_queue_dir, out_name);

  if (rename(dir_path, out_path) < 0) {
    err("testing_queue_lock_entry: rename for %s failed: %s",
        packet_name, os_ErrorMsg());
    return -1;
  }
  if (stat(out_path, &sb) < 0) {
    err("testing_queue_lock_entry: stat for %s failed: %s",
        packet_name, os_ErrorMsg());
    return -1;
  }
  if (!S_ISREG(sb.st_mode)) {
    err("testing_queue_lock_entry: invalid file type of %s", out_path);
    return -1;
  }
  if (sb.st_nlink != 1) {
    err("testing_queue_lock_entry: file %s has been linked several times",
        out_path);
    unlink(out_path);
    return -1;
  }

  if (generic_read_file(&pkt_buf, 0, &pkt_size, 0, 0, out_path, 0) < 0) {
    // no attempt to recover...
    return -1;
  }

  if (run_request_packet_read(pkt_size, pkt_buf, packet) < 0 || !*packet) {
    *packet = 0;
    xfree(pkt_buf); pkt_buf = 0;
    // no attempt to recover either...
    return -1;
  }

  xfree(pkt_buf); pkt_buf = 0;
  pkt_size = 0;

  if ((*packet)->contest_id != contest_id) {
    // contest_id mismatch
    testing_queue_unlock_entry(run_queue_dir, out_path, packet_name);
    *packet = run_request_packet_free(*packet);
    return -1;
  }

  return 0;
}

static int
testing_queue_unlock_entry(
        const unsigned char *run_queue_dir,
        const unsigned char *out_path,
        const unsigned char *packet_name)
{
  path_t dir_path;

  snprintf(dir_path, sizeof(dir_path), "%s/dir/%s",
           run_queue_dir, packet_name);
  if (rename(out_path, dir_path) < 0) {
    err("testing_queue_unlock_entry: rename %s -> %s failed: %s",
        out_path, dir_path, os_ErrorMsg());
    return -1;
  }

  return 0;
}

static int
get_priority_value(int priority_code)
{
  int priority = 0;
  if (priority_code >= '0' && priority_code <= '9') {
    priority = -16 + (priority_code - '0');
  } else if (priority_code >= 'A' && priority_code <= 'V') {
    priority = -6 + (priority_code - 'A');
  }
  return priority;
}

static int
get_priority_code(int priority)
{
  priority += 16;
  if (priority < 0) priority = 0;
  if (priority > 31) priority = 31;
  return b32_digits[priority];
}

int
serve_testing_queue_delete(
        const struct contest_desc *cnts,
        const serve_state_t state,
        const unsigned char *packet_name)
{
  const unsigned char *run_queue_dir = state->global->run_queue_dir;
  path_t out_path;
  path_t out_name;
  struct run_request_packet *packet = 0;
  path_t exe_path;
  struct run_entry re;

  if (testing_queue_lock_entry(cnts->id, run_queue_dir, packet_name,
                               out_name, sizeof(out_name),
                               out_path, sizeof(out_path), &packet) < 0)
    return -1;

  snprintf(exe_path, sizeof(exe_path), "%s/%s%s",
           state->global->run_exe_dir, packet_name, packet->exe_sfx);
  unlink(out_path);
  unlink(exe_path);

  if (run_get_entry(state->runlog_state, packet->run_id, &re) >= 0
      && re.status == RUN_RUNNING
      && re.judge_id == packet->judge_id) {
    run_change_status(state->runlog_state, packet->run_id, RUN_PENDING, 0,-1,0);
  }

  packet = run_request_packet_free(packet);
  return 0;
}

int
serve_testing_queue_change_priority(
        const struct contest_desc *cnts,
        const serve_state_t state,
        const unsigned char *packet_name,
        int adjustment)
{
  const unsigned char *run_queue_dir = state->global->run_queue_dir;
  const unsigned char *run_exe_dir = state->global->run_exe_dir;
  path_t out_path;
  path_t out_name;
  struct run_request_packet *packet = 0;
  path_t new_packet_name;
  path_t exe_path;
  path_t new_exe_path;

  if (testing_queue_lock_entry(cnts->id, run_queue_dir, packet_name,
                               out_name, sizeof(out_name),
                               out_path, sizeof(out_path), &packet) < 0)
    return -1;

  snprintf(new_packet_name, sizeof(new_packet_name), "%s", packet_name);
  new_packet_name[0] = get_priority_code(get_priority_value(new_packet_name[0]) + adjustment);
  if (!strcmp(packet_name, new_packet_name)) {
    packet = run_request_packet_free(packet);
    return 0;
  }

  snprintf(exe_path, sizeof(exe_path), "%s/%s%s",
           run_exe_dir, packet_name, packet->exe_sfx);
  snprintf(new_exe_path, sizeof(new_exe_path), "%s/%s%s",
           run_exe_dir, new_packet_name, packet->exe_sfx);
  if (rename(exe_path, new_exe_path) < 0) {
    err("serve_testing_queue_up: rename %s -> %s failed: %s",
        exe_path, new_exe_path, os_ErrorMsg());
    testing_queue_unlock_entry(run_queue_dir, out_path, packet_name);
    packet = run_request_packet_free(packet);
    return -1;
  }

  testing_queue_unlock_entry(run_queue_dir, out_path, new_packet_name);
  packet = run_request_packet_free(packet);
  return 0;
}

static void
collect_run_packets(const serve_state_t state, strarray_t *vec)
{
  path_t dir_path;
  DIR *d = 0;
  struct dirent *dd;

  memset(vec, 0, sizeof(*vec));
  snprintf(dir_path, sizeof(dir_path), "%s/dir", state->global->run_queue_dir);
  if (!(d = opendir(dir_path))) return;

  while ((dd = readdir(d))) {
    if (!strcmp(dd->d_name, ".") && !strcmp(dd->d_name, "..")) continue;
    xexpand(vec);
    vec->v[vec->u++] = xstrdup(dd->d_name);
  }

  closedir(d); d = 0;
}

int
serve_testing_queue_delete_all(
        const struct contest_desc *cnts,
        const serve_state_t state)
{
  strarray_t vec;
  int i;

  collect_run_packets(state, &vec);
  for (i = 0; i < vec.u; ++i) {
    serve_testing_queue_delete(cnts, state, vec.v[i]);
  }

  xstrarrayfree(&vec);
  return 0;
}

int
serve_testing_queue_change_priority_all(
        const struct contest_desc *cnts,
        const serve_state_t state,
        int adjustment)
{
  strarray_t vec;
  int i;

  collect_run_packets(state, &vec);
  for (i = 0; i < vec.u; ++i) {
    serve_testing_queue_change_priority(cnts, state, vec.v[i], adjustment);
  }

  xstrarrayfree(&vec);
  return 0;
}

/*
 * Local variables:
 *  compile-command: "make"
 *  c-font-lock-extra-types: ("\\sw+_t" "FILE")
 * End:
 */
