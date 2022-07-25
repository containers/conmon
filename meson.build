project('conmon', 'c',
        version : run_command('cat', files('VERSION'),).stdout().strip(),
        license : 'Apache-2.0',
        default_options : [
                'c_std=c99',
        ],
        meson_version : '>= 0.46',
)

git_commit = ''

git = find_program('git', required : false)
if git.found()
        git_commit = run_command(
                git,
                ['--git-dir=@0@/.git'.format(meson.source_root()),
                 'rev-parse', 'HEAD']).stdout().strip()
        git_dirty = run_command(
                git,
                ['--git-dir=@0@/.git'.format(meson.source_root()),
                 'status', '--porcelain', '--untracked-files=no']).stdout().strip()
        if git_dirty != ''
                git_commit = git_commit + '-dirty'
        endif
endif

conf = configuration_data()
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('GIT_COMMIT', git_commit)

add_project_arguments('-Os', '-Wall', '-Werror',
                      '-DVERSION=' + conf.get('VERSION'),
                      '-DGIT_COMMIT=' + conf.get('GIT_COMMIT'),
                      language : 'c')

glib = dependency('glib-2.0')
seccomp = dependency('libseccomp', version : '>= 2.5.2')
if seccomp.found()
  add_project_arguments('-DUSE_SECCOMP=1', language : 'c')
endif

cc = meson.get_compiler('c')
null_dep = dependency('', required : false)
if cc.has_function('dlopen')
  libdl = null_dep
else
  libdl = cc.find_library('dl')
endif

sd_journal = dependency('libsystemd-journal', required : false)
if not sd_journal.found()
	sd_journal = dependency('libsystemd', required : false)
endif
if sd_journal.found()
	add_project_arguments('-DUSE_JOURNALD=1', language : 'c')
endif

executable('conmon',
           ['src/conmon.c',
            'src/config.h',
            'src/cmsg.c',
            'src/cmsg.h',
            'src/ctr_logging.c',
            'src/ctr_logging.h',
            'src/cgroup.c',
            'src/cgroup.h',
            'src/cli.c',
            'src/cli.h',
            'src/conn_sock.c',
            'src/conn_sock.h',
            'src/ctr_exit.c',
            'src/ctr_exit.h',
            'src/ctrl.c',
            'src/ctrl.h',
            'src/ctr_logging.c',
            'src/ctr_logging.h',
            'src/ctr_stdio.c',
            'src/ctr_stdio.h',
            'src/globals.c',
            'src/globals.h',
            'src/close_fds.c',
            'src/close_fds.h',
            'src/oom.c',
            'src/oom.h',
            'src/parent_pipe_fd.c',
            'src/parent_pipe_fd.h',
            'src/runtime_args.c',
            'src/runtime_args.h',
            'src/utils.c',
            'src/utils.h',
            'src/seccomp_notify.c',
            'src/seccomp_notify.h'],
           dependencies : [glib, libdl, sd_journal, seccomp],
           install : true,
           install_dir : join_paths(get_option('libexecdir'), 'podman'),
)
