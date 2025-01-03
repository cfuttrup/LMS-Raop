package Plugins::RaopBridge::Plugin;

use strict;

use base qw(Slim::Plugin::Base);

use File::Spec::Functions;
use FindBin qw($Bin);
use lib catdir($Bin, 'Plugins', 'RaopBridge', 'lib');

use Data::Dumper;
use XML::Simple;

use Slim::Utils::Prefs;
use Slim::Utils::Log;

my $prefs = preferences('plugin.raopbridge');
my $fade_volume;

$prefs->init({ 
	autorun => 1, 
	opts => '', 
	debugs => '', 
	logging => 0, 
	bin => undef, 
	configfile => "raopbridge.xml", 
	profilesURL => initProfilesURL(), 
	autosave => 1, 
	eraselog => 0
});

my $log = Slim::Utils::Log->addLogCategory({
	'category'     => 'plugin.raopbridge',
	'defaultLevel' => 'WARN',
	'description'  => Slim::Utils::Strings::string('PLUGIN_RAOPBRIDGE'),
}); 

sub fade_volume {
	my ($client, $fade, $callback, $callbackargs) = @_;
	return $fade_volume->($client, $fade, $callback, $callbackargs) if $fade > 0 || $client->modelName !~ /RaopBridge/;
	return $callback->($callbackargs) if $callback;
}

sub initPlugin {
	my $class = shift;

	$class->SUPER::initPlugin(@_);
	
	# this is hacky but I won't redefine a whole player model just for this	
	require Slim::Player::SqueezePlay;
	$fade_volume = \&Slim::Player::SqueezePlay::fade_volume;
	*Slim::Player::SqueezePlay::fade_volume = \&fade_volume;
	
	*Slim::Utils::Log::raopbridgeLogFile = sub { Plugins::RaopBridge::Squeeze2raop->logFile; };
		
	require Plugins::RaopBridge::Squeeze2raop;	
	require	Plugins::RaopBridge::Pairing;
	
	if ($prefs->get('autorun')) {
		Plugins::RaopBridge::Squeeze2raop->start;
	}
	
	if (!$::noweb) {
		require Plugins::RaopBridge::Settings;
		Plugins::RaopBridge::Settings->new;
		# there is a bug in LMS where the "content-type" set in handlers is ignored, only extension matters (and is html by default)
		Slim::Web::Pages->addPageFunction("raopbridge-log", \&Plugins::RaopBridge::Squeeze2raop::logHandler);
		Slim::Web::Pages->addPageFunction("raopbridge-config.xml", \&Plugins::RaopBridge::Squeeze2raop::configHandler);
		Slim::Web::Pages->addPageFunction("raopbridge-guide", \&Plugins::RaopBridge::Squeeze2raop::guideHandler);
	}
	
	$log->warn(Dumper(Slim::Utils::OSDetect::details()));
}

sub initProfilesURL {
	my $file = catdir(Slim::Utils::PluginManager->allPlugins->{'RaopBridge'}->{'basedir'}, 'install.xml');
	return XMLin($file, ForceArray => 0, KeepRoot => 0, NoAttr => 0)->{'profilesURL'};
}

sub shutdownPlugin {
	if ($prefs->get('autorun')) {
		Plugins::RaopBridge::Squeeze2raop->stop;
	}
}

1;
