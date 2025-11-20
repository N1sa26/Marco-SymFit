// create a tmpfs to make our fuzzing faster
// only for :
// inputs directory and .cur_input, .socket file

use libc;
use std::{fs, os::unix::fs::symlink, path::Path, env};
use fastgen_common::defs;

static LINUX_TMPFS_DIR: &str = "/dev/shm";

pub fn create_tmpfs_dir(target: &Path) {
    if env::var(defs::PERSIST_TRACK_FILES).is_ok() {
        if let Err(e) = fs::create_dir_all(&target) {
            if e.kind() != std::io::ErrorKind::AlreadyExists { panic!("create_dir_all failed: {:?}", e); }
        }
        return;
    }
    let shm_dir = Path::new(LINUX_TMPFS_DIR);
    if shm_dir.is_dir() {
        // support tmpfs
        // create a dir in /dev/shm, then symlink it to target
        let pid = unsafe { libc::getpid() as usize };
        let dir_name = format!("angora_tmp_{}", pid);
        let tmp_dir = shm_dir.join(dir_name);
        if let Err(e) = fs::create_dir_all(&tmp_dir) {
            if e.kind() != std::io::ErrorKind::AlreadyExists { panic!("create_dir_all shm failed: {:?}", e); }
        }
        if target.exists() {
            if target.is_dir() { let _ = fs::remove_dir_all(&target); } else { let _ = fs::remove_file(&target); }
        }
        symlink(&tmp_dir, target).unwrap();
    } else {
        // not support
        warn!(
            "System does not have {} directory! Can't use tmpfs.",
            LINUX_TMPFS_DIR
        );
        if let Err(e) = fs::create_dir_all(&target) {
            if e.kind() != std::io::ErrorKind::AlreadyExists { panic!("create_dir_all no-tmpfs failed: {:?}", e); }
        }
    }
}

pub fn clear_tmpfs_dir(target: &Path) {
    if env::var(defs::PERSIST_TRACK_FILES).is_ok() {
        return;
    }
    if target.exists() {
        fs::remove_file(target).unwrap();
    }
    let shm_dir = Path::new(LINUX_TMPFS_DIR);
    if shm_dir.is_dir() {
        // support tmpfs
        let pid = unsafe { libc::getpid() as usize };
        let dir_name = format!("angora_tmp_{}", pid);
        let tmp_dir = shm_dir.join(dir_name);
        fs::remove_dir_all(&tmp_dir).unwrap();
    }
}
