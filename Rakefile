require 'lib/fusefs'
require 'rake/gempackagetask'

Spec = Gem::Specification.new do |s|
  s.name = 'FuseFS'
  s.version = '0.7.0'
  s.summary = 'Define filesystems entirely in Ruby.'
  s.description = <<EOF
FuseFS lets ruby programmers define filesystems entirely in Ruby.
That is - with FuseFS, you can now create a mounted filesystem entirely
defined in Ruby! Included are proof of concept filesystems:
SQL table mappings, YAML filesystem, and more!
EOF
  s.files = FileList[
    'API.txt', 'Changes.txt', 'COPYRIGHT', 'ext/**/*', 'lib/**/*', 'Makefile',
    'README.txt', 'sample/**/*', 'setup.rb', 'TODO'
  ]
  s.extensions << 'ext/extconf.rb'
  s.require_path = 'lib'
  s.autorequire = 'fusefs'

  s.author = 'Greg Millam'
  s.email = 'walker@deafcode.com'
  s.rubyforge_project = 'FuseFS'

end

Rake::GemPackageTask.new(Spec) do |p|
end

task :export do
  dirname = "fusefs-#{FuseFS::VERSION}"
  sh %{cvs export -D now -d #{dirname} PageTemplate}
end

task :all do
  ruby %{setup.rb config}
  ruby %{setup.rb setup}
end

task :default => [ :all ]

task :install do
  ruby %{setup.rb install}
end

task :clean do
  ruby %{setup.rb clean}
end
