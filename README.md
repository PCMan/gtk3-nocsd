#gtk3-nocsd

gtk3-nocsd is a small module used to disable the client side decoration of gtk+ 3.

##Introduction:
Since gtk+ 3.10, its developers added a so-called header bar or custom title bar.
With this and the client-side decoration, the original title bar and window border
provided by the window manager are disabled by gtk+. This makes all gtk+ 3 programs
look alike. Even worse, this may break some window manager or compositors.

Unfortunately, the gtk+ developers decided to be against the 
existing standards and provide "no option" to turn it off.

Luckily, with gtk3-nocsd, we still have a way to (partially) turn it off. Window manager (title bar and window border) can be re-enabled.

##Preview:
This is how the gtk3 windows look like before and after using `gtk3-nocsd`.

![](http://i.imgur.com/7rMRozy.png)

![](http://i.imgur.com/Ic4pUs9.png)

#How to use:

* Make sure your gtk+ version is either older than 3.16.1 or newer than
3.17.3.
* Install necessary packages.  
On Debian based systems, install `pkg-config`, `libgtk-3-dev`, `libgirepository1.0-dev`.  
On Rpm based distros, install `pkgconfig`, `gtk3-devel`, `gtk+-devel`, `gobject-introspection-devel`.
* Build the code. Run `make` from command line.
After this you'll have the files `gtk3-nocsd`and `libgtk3-nocsd.so.0` in the same directory.  
* Now to run individual gtk3 apps(say gedit) using this hack, use the command `./gtk3-nocsd gedit` from the same directory.
* To have all gtk3 apps(of current user) use this hack, export some environment variables in your `~/.bashrc`.  

    > export GTK_CSD=0  
    > export LD_PRELOAD=<"full path" of your libgtk3-nocsd.so.0 file>

* Re-login to make the environment variables take effect.

* Hooray! GTK+ 3 client-side decoration is dead now.

#How it works:

I use $LD_PRELOAD to override several gdk and glib/gobject APIs to
intercept related calls gtk+ 3 uses to setup CSD.
While gtk+ 3 is trying to initialize CSD, I let it think that there
is no compositor available, so CSD fail to start.
Actually, we never disable X compositing, so other parts are not affected.
This hack to my knowledge is the most specific way to disable CSD
without obvious side effects.

