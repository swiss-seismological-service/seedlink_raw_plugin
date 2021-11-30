* template: $template
plugin $seedlink.source.id cmd="$seedlink.plugin_dir/raw_plugin -s $sources.raw.host -p $sources.raw.port -c $sources.raw.stream -m $sources.raw.componentMap"
             timeout = 600
             start_retry = 60
             shutdown_wait = 10

