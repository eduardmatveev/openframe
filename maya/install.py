"""Install the independent OpenFrame adapter and its dedicated Maya shelf."""
import datetime
import importlib
import os
from pathlib import Path
import shutil
import stat
import sys
import uuid


def reject_links(path):
    path = Path(path)
    for parent in (path,) + tuple(path.parents):
        if parent.exists() and getattr(parent.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise RuntimeError('Installation path must not contain links: ' + str(parent))
        if parent.is_symlink():
            raise RuntimeError('Installation path must not contain links: ' + str(parent))


def install():
    import maya.cmds as cmds
    import maya.mel as mel
    source = Path(__file__).resolve().parent
    scripts = Path(cmds.internalVar(userAppDir=True)) / 'scripts'
    reject_links(scripts)
    packages = ('openframe_maya', 'openframe_compat')
    for name in packages:
        if not (source / name).is_dir() or not list((source / name).glob('*.py')):
            raise RuntimeError('Extract the whole OpenFrame archive before installing: missing ' + name)
        reject_links(scripts / name)
        if (scripts / name).exists():
            for path in (scripts / name).rglob('*'):
                reject_links(path)
    scripts.mkdir(parents=True, exist_ok=True)
    backup = scripts / 'OpenFrameBackups' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    reject_links(backup)
    for name in packages:
        destination = scripts / name
        if destination.exists():
            backup.mkdir(parents=True, exist_ok=True)
            shutil.copytree(str(destination), str(backup / name))
        destination.mkdir(exist_ok=True)
        for path in (source / name).rglob('*.py'):
            reject_links(path)
            relative = path.relative_to(source / name)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(path), str(target))
    # Stop the old timer/callbacks before loading the updated adapter.
    old_ui = sys.modules.get('openframe_maya.ui')
    controller = getattr(old_ui, '_controller', None)
    if controller:
        controller.close()
        if cmds.window(controller.window, exists=True):
            cmds.deleteUI(controller.window)
    for name in list(sys.modules):
        if any(name == package or name.startswith(package + '.') for package in packages):
            del sys.modules[name]
    sys.path.insert(0, str(scripts)) if str(scripts) not in sys.path else None
    importlib.invalidate_caches()
    shelf_parent = mel.eval('$ofShelfParent = $gShelfTopLevel')
    shelf = 'OpenFrame'
    if not cmds.shelfLayout(shelf, exists=True):
        shelf = cmds.shelfLayout(shelf, parent=shelf_parent)
    def button(name, **options):
        control = shelf + '|' + name
        if cmds.shelfButton(control, exists=True):
            cmds.shelfButton(control, edit=True, **options)
        else:
            cmds.shelfButton(name, parent=shelf, **options)
    button('openframeAdapterButton', label='OF Maya', annotation='OpenFrame: Maya synchronization and playblast',
           image='commandButton.png', imageOverlayLabel='OF Maya', sourceType='python',
           command='import openframe_maya\nopenframe_maya.show()')
    executable = source.parent / 'OpenFrame.exe'
    if not executable.is_file():
        executable = source.parent / 'dist' / 'OpenFrame-local' / 'OpenFrame.exe'
    if executable.is_file():
        button('openframePlayerButton', label='OpenFrame', annotation='Launch OpenFrame player',
               image='commandButton.png', imageOverlayLabel='OF', sourceType='python',
               command='import subprocess\nsubprocess.Popen([' + repr(str(executable)) + '])')
    shelf_dir = Path(cmds.internalVar(userShelfDir=True))
    shelf_dir.mkdir(parents=True, exist_ok=True)
    cmds.saveShelf(shelf, str(shelf_dir / 'shelf_OpenFrame'))
    cmds.shelfTabLayout(shelf_parent, edit=True, selectTab=shelf)
    message = 'OpenFrame installed. Shelf: OpenFrame. Start the player, then click OF Maya and Connect.'
    cmds.inViewMessage(amg=message, pos='midCenter', fade=True)
    print(message + '\nScripts: ' + str(scripts))
    return {'scripts': str(scripts), 'shelf': shelf, 'player': str(executable) if executable.is_file() else None,
            'backup': str(backup) if backup.exists() else None}


if __name__ == '__main__':
    install()
