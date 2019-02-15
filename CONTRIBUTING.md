Patches to igt-gpu-tools are very much welcome, we really want this to be the
universal set of low-level tools and testcases for kernel graphics drivers
on Linux and similar platforms. So please bring on porting patches, bugfixes,
improvements for documentation and new tools and testcases.

A short list of contribution guidelines:

- Please submit patches formatted with git send-email/git format-patch or
  equivalent to:

      Development mailing list for IGT GPU Tools <igt-dev@lists.freedesktop.org>

  For patches affecting the driver directly please "cc" the appropriate driver
  mailing list and make sure you are using:

      --subject-prefix="PATCH i-g-t"

  so IGT patches are easily identified in the massive amount mails on driver's
  mailing list. To ensure this is always done, meson.sh (and autogen.sh) will
  run:

      git config format.subjectprefix "PATCH i-g-t"

  on its first invocation.

- igt-gpu-tools is MIT licensed and we require contributions to follow the
  developer's certificate of origin: http://developercertificate.org/

- When submitting new testcases please follow the naming conventions documented
  in the generated documentation. Also please make full use of all the helpers
  and convenience macros provided by the igt library. The semantic patch
  lib/igt.cocci can help with the more automatic conversions.

- Patches need to be reviewed on the mailing list. Exceptions only apply for
  testcases and tooling for drivers with just a single contributor (e.g. vc4).
  In this case patches must still be submitted to the mailing list first.
  Testcase should preferably be cross-reviewed by the same people who write and
  review the kernel feature itself.

- When patches from new contributors (without commit access) are stuck, for
  anything related to the regular releases, issues with packaging and
  integrating platform support or any other igt-gpu-tools issues, please
  contact one of the maintainers (listed in the MAINTAINERS file) and cc the
  igt-dev mailing list.

- Changes to the testcases are automatically tested. Take the results into
  account before merging.

Commit rights
-------------

Commit rights will be granted to anyone who requests them and fulfills the
below criteria:

- Submitted a few (5-10 as a rule of thumb) non-trivial (not just simple
  spelling fixes and whitespace adjustment) patches that have been merged
  already.

- Are actively participating on discussions about their work (on the mailing
  list or IRC). This should not be interpreted as a requirement to review other
  peoples patches but just make sure that patch submission isn't one-way
  communication. Cross-review is still highly encouraged.

- Will be regularly contributing further patches. This includes regular
  contributors to other parts of the open source graphics stack who only
  do the oddball rare patch within igt itself.

- Agrees to use their commit rights in accordance with the documented merge
  criteria, tools, and processes.

Create a gitlab account at https://gitlab.freedesktop.org/ and apply
for access to the IGT gitlab project,
http://gitlab.freedesktop.org/drm/igt-gpu-tools and please ping the
maintainers if your request is stuck.

Committers are encouraged to request their commit rights get removed when they
no longer contribute to the project. Commit rights will be reinstated when they
come back to the project.

Maintainers and committers should encourage contributors to request commit
rights, especially junior contributors tend to underestimate their skills.

Code of Conduct
---------------

Please be aware the fd.o Code of Conduct also applies to igt:

https://www.freedesktop.org/wiki/CodeOfConduct/

See the MAINTAINERS file for contact details of the igt maintainers.

Abuse of commit rights, like engaging in commit fights or willfully pushing
patches that violate the documented merge criteria, will also be handled through
the Code of Conduct enforcement process.

Happy hacking!
