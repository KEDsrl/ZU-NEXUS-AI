# media/ — fallback video and panel logos

## dron_720p.y4m — fallback video

Copy `dron_720p.y4m` here. When present, the `dxpose-qt` package installs it to

    /usr/share/dxpose-qt/media/dron_720p.y4m

and the application plays it instead of the camera when:

- the video chain is not detected, or cannot be configured at startup;
- the camera pipeline fails (v4l2src, not-negotiated, STREAMON...);
- the camera stops delivering frames (4 s) or delivers none within 20 s of start.

The **Retry camera** button in the panel switches back to the camera.

y4m is uncompressed (about 41 MB/s at 720p30), so keep the clip short or raise
`BR2_TARGET_ROOTFS_EXT2_SIZE` in the defconfig. The file is listed in
`.gitignore`: distribute it outside the repository.

## logos/ — panel logos

Every `.png` in `media/logos/` is installed to `/usr/share/dxpose-qt/logos/`
(the target directory is wiped first, so removing a logo here removes it from the
image as well). The application shows only the logos it finds, sorted
alphabetically by file name, all at the same height and on a single row: 34 px
when there is room, less depending on how many logos are present (about 30 px
with four).

After adding or removing files:

    ./build.sh dxpose-qt-rebuild && ./build.sh

Files can also be added or removed directly in `/usr/share/dxpose-qt/logos/` on
the target, followed by `systemctl restart dxpose-qt`.

Recommended: transparent background, cropped to the content, at least 68 px tall.
