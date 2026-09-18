# github.com/opencv/opencv no longer has a master branch: only 2.4, 3.4, 4.x
# and 5.x are left. meta-oe's recipe fetches the pinned 4.5.5 release commit
# through branch=master, so do_fetch now fails with
#
#   Unable to find revision dad26339a975b49cfb6c7dbe4bd5276c9dcb36e2
#   in branch master even from upstream
#
# The commit is still there, and is an ancestor of 4.x. Since the recipe pins
# it, the branch is only used to validate the revision, which is pointless for
# a fixed SRCREV: nobranch=1 drops that check and fetches the commit directly,
# and unlike branch=4.x it does not break again the next time upstream retires
# a branch. Only the opencv URL is touched; opencv_contrib and opencv_3rdparty
# still carry the branches the recipe names.
#
# Nothing to send upstream: kirkstone's meta-openembedded has not been touched
# here since 2024 and carries the same breakage.

python () {
    src = d.getVar("SRC_URI")
    fixed = src.replace("github.com/opencv/opencv.git;name=opencv;branch=master",
                        "github.com/opencv/opencv.git;name=opencv;nobranch=1")
    if fixed == src:
        bb.warn("opencv bbappend: the opencv git URL is not the expected one, "
                "left untouched: the fetch may still fail")
    d.setVar("SRC_URI", fixed)
}
