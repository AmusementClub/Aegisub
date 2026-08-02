if [ -z "$srcdir" ] && [ $# -gt 0 ]; then
  srcdir=$1
fi

get_build_time_utc() {
  if [ -n "${SOURCE_DATE_EPOCH}" ]; then
    # GNU date uses -d @epoch; BSD date uses -r epoch
    date -u -d "@${SOURCE_DATE_EPOCH}" +%Y%m%dT%H%M%SZ 2>/dev/null \
      || date -u -r "${SOURCE_DATE_EPOCH}" +%Y%m%dT%H%M%SZ 2>/dev/null \
      || date -u +%Y%m%dT%H%M%SZ
  else
    date -u +%Y%m%dT%H%M%SZ
  fi
}

build_time_utc=$(get_build_time_utc)

# If no git repo try to read from the existing git_version.h, for building from tarballs
if ! test -d "${srcdir}/.git"; then
  version_h_path="${srcdir}/build/git_version.h"
  if test -f "${version_h_path}"; then
    while read line; do
      set -- $line
      export $2=$(echo $3 | sed 's/"//g')
    done < "${version_h_path}"
    if test x$BUILD_GIT_VERSION_NUMBER != x -a x$BUILD_GIT_VERSION_STRING != x; then
      export VERSION_SOURCE="from cached git_version.h"
      return 0
    else
      echo "invalid git_version.h"
      exit 2
    fi
  elif [ -z "$FORCE_GIT_VERSION" ]; then
    echo "git repo not found and no cached git_version.h - use FORCE_GIT_VERSION to override"
    exit 2
  fi
fi

last_svn_revision=6962
last_svn_hash="16cd907fe7482cb54a7374cd28b8501f138116be"

if git rev-parse --verify "${last_svn_hash}^{commit}" >/dev/null 2>&1; then
  commit_count=$(git rev-list --count "${last_svn_hash}..HEAD" 2>/dev/null || echo "")
  if [ -z "$commit_count" ]; then
    echo "warning: could not count commits since ${last_svn_hash}; version number will be 0" >&2
    git_revision=0
  else
    git_revision=$(expr $last_svn_revision + $commit_count)
    if [ "x$git_revision" = x6962 ]; then
      git_revision=0
    fi
  fi
else
  echo "warning: missing history for ${last_svn_hash} (shallow clone?). Version number will be 0. Use fetch-depth: 0 in CI." >&2
  git_revision=0
fi

git_version_str=${FORCE_GIT_VERSION:-$(git describe --exact-match 2> /dev/null)}
installer_version='0.0.0'
resource_version='0, 0, 0'
if test x$git_version_str != x; then
  git_version_str="${git_version_str##v}"
  tagged_release=1
  if [ $(echo $git_version_str | grep '[0-9]\.[0-9]\.[0-9]') ]; then
    installer_version=$git_version_str
    resource_version=$(echo $git_version_str | sed 's/\./, /g')
  fi
else
  git_branch="$(git symbolic-ref HEAD 2> /dev/null)" || git_branch="(unnamed branch)"
  git_branch="${git_branch##refs/heads/}"
  git_hash=$(git rev-parse --short HEAD)

  git_version_str="${git_revision}-${git_branch}-${git_hash}-${build_time_utc}"
  tagged_release=0
fi


new_version_h="\
#define BUILD_GIT_VERSION_NUMBER ${git_revision}
#define BUILD_GIT_VERSION_STRING \"${git_version_str}\"
#define BUILD_GIT_BUILD_TIME_UTC \"${build_time_utc}\"
#define TAGGED_RELEASE ${tagged_release}
#define INSTALLER_VERSION \"${installer_version}\"
#define RESOURCE_BASE_VERSION ${resource_version}"

# may not exist yet for out of tree builds
mkdir -p build
version_h_path="build/git_version.h"

# Write it only if it's changed to avoid spurious rebuilds
# This bizzare comparison method is due to that newlines in shell variables are very exciting
case "$(cat ${version_h_path} 2> /dev/null)"
in
  "${new_version_h}");;
  *) echo "${new_version_h}" > "${version_h_path}"
esac

export BUILD_GIT_VERSION_NUMBER="${git_revision}"
export BUILD_GIT_VERSION_STRING="${git_version_str}"
export BUILD_GIT_BUILD_TIME_UTC="${build_time_utc}"
export VERSION_SOURCE="from git"
