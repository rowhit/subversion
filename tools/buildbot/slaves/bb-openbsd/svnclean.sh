#!/bin/sh
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#

set -e
set -x

(test -d ../build/.svn && svn cleanup ../build)
(test -h ../svn-trunk || ln -s build ../svn-trunk)
for i in $(jot - 6 12); do
  (test -h ../svn-1.${i}.x || ln -s build ../svn-1.${i}.x)
done
lastchangedrev="$(svn info --show-item=last-changed-revision ../../unix-build/Makefile.svn)"
svn cleanup ../../unix-build
svn update ../../unix-build
newlastchangedrev="$(svn info --show-item=last-changed-revision ../../unix-build/Makefile.svn)"
(test -h ../GNUmakefile || ln -s ../unix-build/Makefile.svn ../GNUmakefile)
# always rebuild svn, but only rebuild dependencies if Makefile.svn has changed
url="$(svn info --show-item url)"
branch="${url##*/}"
if [ "$lastchangedrev" != "$newlastchangedrev" ]; then
  (cd .. && gmake BRANCH="$branch" reset clean)
else
  (cd .. && gmake BRANCH="$branch" svn-reset svn-bindings-reset svn-clean)
fi
rm -f tests.log* fails.log*
