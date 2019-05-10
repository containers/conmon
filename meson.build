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

executable('conmon',
           ['src/conmon.c',
            'src/config.h',
            'src/cmsg.c',
            'src/cmsg.h'],
           dependencies : [glib],
           install : true,
           install_dir : join_paths(get_option('libexecdir'), 'podman'),
)

